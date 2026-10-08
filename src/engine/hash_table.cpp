#include "engine/hash_table.h"

#include <utility>

namespace stratum::engine {

void HashTable::set(std::string key, std::string value, int64_t expiry) {
    entries_[std::move(key)] = Entry{std::move(value), expiry};
}

std::optional<std::string> HashTable::get(const std::string& key, int64_t now_ms) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    if (is_expired(it->second, now_ms)) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool HashTable::remove(const std::string& key, int64_t now_ms) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (is_expired(it->second, now_ms)) {
        entries_.erase(it);
        return false;
    }
    entries_.erase(it);
    return true;
}

bool HashTable::exists(const std::string& key, int64_t now_ms) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (is_expired(it->second, now_ms)) {
        entries_.erase(it);
        return false;
    }
    return true;
}

bool HashTable::set_expiry(const std::string& key, int64_t deadline_ms, int64_t now_ms) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    if (is_expired(it->second, now_ms)) {
        entries_.erase(it);
        return false;
    }
    it->second.expires_at = deadline_ms;
    return true;
}

TtlResult HashTable::get_ttl(const std::string& key, int64_t now_ms) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return {TtlStatus::Missing, 0};
    }
    if (is_expired(it->second, now_ms)) {
        entries_.erase(it);
        return {TtlStatus::Missing, 0};
    }
    if (it->second.expires_at == NO_EXPIRY) {
        return {TtlStatus::NoExpiry, 0};
    }
    int64_t remaining_ms = it->second.expires_at - now_ms;
    return {TtlStatus::Ok, remaining_ms};
}

} // namespace stratum::engine
