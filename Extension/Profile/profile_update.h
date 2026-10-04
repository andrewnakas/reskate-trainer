#pragma once
#include "database_codec.h"
#include "profile_internal.h"
#include <set>
#include <type_traits>

namespace dingosdk::profile {
// Caller holds Store::mutex_. Edits copy only the affected record. Old map nodes
// are retained until SQLite commits; any exception restores them without allocation.
class Store::Update {
    struct Undo {
        const void* owner;
        std::string key;
        Undo(const void* owner_value, std::string key_value) : owner(owner_value), key(std::move(key_value)) {}
        virtual ~Undo() = default;
        virtual void apply() = 0;
        virtual void rollback() noexcept = 0;
        virtual bool changed() const = 0;
    };
    template<class Map> struct MapUndo final : Undo {
        Map& map;
        typename Map::node_type old, replacement;
        typename Map::iterator current;
        bool applied{};
        MapUndo(Map& target, const std::string& key_value) : Undo(&target, key_value), map(target) {
            Map staged;
            auto found = map.find(this->key);
            staged.emplace(this->key, found == map.end() ? typename Map::mapped_type{} : found->second);
            replacement = staged.extract(staged.begin());
        }
        void apply() override {
            old = map.extract(this->key);
            current = map.insert(std::move(replacement)).position;
            applied = true;
        }
        bool changed() const override { return old.empty() || old.mapped() != current->second; }
        void rollback() noexcept override {
            if (!applied) return;
            map.erase(current);
            if (!old.empty()) map.insert(std::move(old));
        }
    };
    Store& store_;
    std::vector<std::unique_ptr<Undo>> undo_;
    std::map<std::string, std::set<std::string>, std::less<>> records_;
    struct JsonEdit { Json* root; std::vector<std::string> path; };
    std::vector<JsonEdit> json_edits_;
    bool committed_{};
    template<class Map> auto& edit(Map& map, const std::string& key) {
        for (const auto& entry : undo_)
            if (entry->owner == &map && entry->key == key) return map.at(key);
        auto entry = std::make_unique<MapUndo<Map>>(map, key);
        auto* operation = entry.get();
        undo_.push_back(std::move(entry));
        operation->apply();
        return map.at(key);
    }
    void encode(Json& patch);
public:
    explicit Update(Store& store) : store_(store) {}
    Update(const Update&) = delete;
    Update& operator=(const Update&) = delete;
    ~Update() {
        if (!committed_) for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) (*it)->rollback();
    }
    template<class Map> auto& record(Map& map, std::string_view key, std::string_view table) {
        records_[std::string(table)].emplace(key);
        return edit(map, std::string(key));
    }
    Json& json(Json& root, std::initializer_list<std::string_view> path, bool container = false) {
        detail::require(path.size() != 0, "Empty profile update path");
        JsonEdit tagged{&root, {}};
        for (auto key : path) tagged.path.emplace_back(key);
        json_edits_.push_back(std::move(tagged));
        auto* node = &root;
        std::size_t index{};
        for (auto key : path) {
            detail::require(node->is_object(), "Invalid profile update object");
            const bool last = ++index == path.size();
            if (!node->contains(key) || (last && !container)) {
                auto& next = edit(node->items(), std::string(key));
                if ((!last || container) && next.is_null()) next = Json::object();
                node = &next;
            } else node = &node->at(key);
        }
        if (container) detail::require(node->is_object(), "Invalid profile collection");
        return *node;
    }
    void commit() {
        if (std::none_of(undo_.begin(), undo_.end(), [](const auto& item) { return item->changed(); })) {
            committed_ = true; return;
        }
        auto& value = store_.value_;
        detail::require(value.revision != UINT64_MAX, "Profile revision overflow");
        detail::require(value.bool_options.size() + value.play_events.size() + value.quests.size() +
            value.entitlements.size() + value.neighborhood_ranks.size() <= detail::max_records,
            "Too many local profile records");
        Json patch = Json::object();
        encode(patch);
        auto metadata = database::singleton(store_.database_->document(), "profile_meta");
        metadata[1] = std::to_string(value.revision + 1);
        const auto metadata_key = storage::row_key(metadata, 1);
        patch["profile_meta"][metadata_key] = std::move(metadata);
        store_.database_->patch(std::move(patch));
        ++value.revision;
        committed_ = true;
        // Every setting, option and outfit is saved through here. Without this the settings
        // hooks keep answering from the values they cached before the save.
        Store::note_change();
    }
};
}
