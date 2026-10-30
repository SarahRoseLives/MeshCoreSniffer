// tiles.h - OSM/Esri slippy-map tiles and a pannable/zoomable map view
// (adapted from OpenITC).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "model.h"

namespace mcs::gui {

class TileCache {
public:
    TileCache() = default;
    ~TileCache();

    TileCache(const TileCache&) = delete;
    TileCache& operator=(const TileCache&) = delete;

    void init(const std::string& cache_dir, const std::string& url_template);
    void set_online(bool online) { online_.store(online); }
    bool online() const { return online_.load(); }

    ImTextureID texture(int z, int x, int y);
    void pump();

    int ready() const { return ready_count_.load(); }
    int failed() const { return failed_count_.load(); }
    int pending() const { return pending_count_.load(); }
    const std::string& attribution() const { return attribution_; }

private:
    struct Key {
        int z = 0, x = 0, y = 0;
        bool operator==(const Key& o) const { return z == o.z && x == o.x && y == o.y; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const
        {
            return (size_t)k.z * 73856093u ^ (size_t)k.x * 19349663u ^
                   (size_t)k.y * 83492791u;
        }
    };
    enum State { Pending, Loading, Ready, Failed };
    struct Tile {
        State state = Pending;
        unsigned gl = 0;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        double retry_at = 0.0;
    };

    void worker();
    void enqueue(const Key& k);
    std::string path_for(const Key& k) const;
    std::string url_for(const Key& k) const;
    bool load_disk(const Key& k, std::vector<uint8_t>& png);
    void save_disk(const Key& k, const std::vector<uint8_t>& png);

    std::string dir_;
    std::string url_;
    std::string provider_ = "tiles";
    std::string attribution_ = "OpenStreetMap contributors";
    std::string ua_;
    double next_net_ = 0.0;
    double net_block_until_ = 0.0;
    int consec_fail_ = 0;
    std::thread th_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<Key> q_;
    std::unordered_map<Key, Tile, KeyHash> tiles_;
    bool stop_ = false;
    std::atomic<bool> online_{true};
    std::atomic<int> ready_count_{0};
    std::atomic<int> failed_count_{0};
    std::atomic<int> pending_count_{0};
};

class MapView {
public:
    bool draw(TileCache& cache, const ImVec2& size, const std::vector<MapMarker>& markers,
              std::string& clicked_id);
    void fit(const std::vector<MapMarker>& markers);
    void center_on(double lat, double lon, int zoom = 13);

    void zoom_in() { if (zoom_ < 18) ++zoom_; }
    void zoom_out() { if (zoom_ > 2) --zoom_; }
    int zoom() const { return zoom_; }
    double center_lat() const;
    double center_lon() const;

private:
    double u_ = 0.2266;
    double v_ = 0.3804;
    int zoom_ = 5;
    float drag_accum_ = 0.0f;
};

} // namespace mcs::gui
