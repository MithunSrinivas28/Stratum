#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace stratum::engine {

/// Sentinel constant representing a persistent entry that never expires.
inline constexpr int64_t NO_EXPIRY = -1;

/// Represents an in-memory storage entry. Expiry lives directly in the entry.
struct Entry {
    std::string value;
    int64_t expires_at = NO_EXPIRY;
};

/// Status for get_ttl queries.
enum class TtlStatus {
    Missing,   // Key does not exist or was expired and removed
    NoExpiry,  // Key exists and is persistent (has NO_EXPIRY)
    Ok         // Key exists with an active deadline; remaining_ms holds difference
};

/// Result of get_ttl.
struct TtlResult {
    TtlStatus status;
    int64_t remaining_ms = 0;

    bool operator==(const TtlResult& other) const {
        return status == other.status && remaining_ms == other.remaining_ms;
    }
};

/// Single-threaded in-memory hash table storage with lazy expiration.
///
/// Keys are stored in an internal std::unordered_map<std::string, Entry>.
/// Every read/write operation accepts an explicit now_ms timestamp, enabling
/// fully deterministic time control without reading the clock inside storage.
class HashTable {
public:
    static constexpr int64_t NO_EXPIRY = stratum::engine::NO_EXPIRY;

    HashTable() = default;

    /// Create or replace key with value and optional absolute expiry deadline in Unix ms.
    /// Replaces any previous expiry on the key (defaults to NO_EXPIRY).
    void set(std::string key, std::string value, int64_t expiry = NO_EXPIRY);

    /// Retrieve key value. If expired (now_ms >= expires_at), lazily deletes
    /// the entry and returns std::nullopt.
    std::optional<std::string> get(const std::string& key, int64_t now_ms);

    /// Remove key from table.
    /// Returns true only if the key existed and was alive.
    /// If the key was expired, it is deleted and returns false.
    bool remove(const std::string& key, int64_t now_ms);

    /// Check if key exists and is alive.
    /// If expired, lazily deletes the entry and returns false.
    bool exists(const std::string& key, int64_t now_ms);

    /// Update the expiration deadline for an existing key.
    /// Returns false if key is missing or already expired.
    bool set_expiry(const std::string& key, int64_t deadline_ms, int64_t now_ms);

    /// Get time-to-live status and remaining milliseconds.
    /// Returns Missing if key is missing or expired (and deletes expired key).
    /// Returns NoExpiry if key is persistent.
    /// Returns Ok with positive remaining_ms if key is alive with an expiration deadline.
    TtlResult get_ttl(const std::string& key, int64_t now_ms);

    /// Number of raw entries currently in the hash table (including any not yet lazily cleared).
    size_t raw_size() const noexcept { return entries_.size(); }

    /// Clear all entries.
    void clear() noexcept { entries_.clear(); }

private:
    std::unordered_map<std::string, Entry> entries_;

    /// Helper to test if an entry is expired relative to now_ms.
    static bool is_expired(const Entry& entry, int64_t now_ms) noexcept {
        return entry.expires_at != NO_EXPIRY && now_ms >= entry.expires_at;
    }
};

} // namespace stratum::engine
