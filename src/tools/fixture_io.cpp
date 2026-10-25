#include "fixture_io.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fzio {
namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

template <class T>
std::vector<T> read_binary(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::streamsize bytes = in.tellg();
    if (bytes < 0 || bytes % static_cast<std::streamsize>(sizeof(T)) != 0)
        throw std::runtime_error("size is not a multiple of the element size: " + path);
    std::vector<T> out(static_cast<size_t>(bytes / static_cast<std::streamsize>(sizeof(T))));
    in.seekg(0);
    if (!out.empty()) {
        in.read(reinterpret_cast<char*>(out.data()), bytes);
        if (!in || in.gcount() != bytes) throw std::runtime_error("short read: " + path);
    }
    return out;  // the layout is little-endian, as are all supported hosts
}

// Minimal extraction from the flat JSON headers of FORMAT.md: finds "key": and returns the raw
// token after it (a number, null, a quoted string without quotes, or a [...] array text).
std::string json_raw(const std::string& doc, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = doc.find(needle);
    if (pos == std::string::npos) throw std::runtime_error("missing key " + key);
    pos = doc.find(':', pos + needle.size());
    if (pos == std::string::npos) throw std::runtime_error("malformed key " + key);
    ++pos;
    while (pos < doc.size() && std::isspace(static_cast<unsigned char>(doc[pos]))) ++pos;
    if (pos >= doc.size()) throw std::runtime_error("malformed value for " + key);
    if (doc[pos] == '"') {
        const size_t end = doc.find('"', pos + 1);
        if (end == std::string::npos) throw std::runtime_error("unterminated string for " + key);
        return doc.substr(pos + 1, end - pos - 1);
    }
    if (doc[pos] == '[') {
        const size_t end = doc.find(']', pos);
        if (end == std::string::npos) throw std::runtime_error("unterminated array for " + key);
        return doc.substr(pos, end - pos + 1);
    }
    size_t end = pos;
    while (end < doc.size() && doc[end] != ',' && doc[end] != '}' && doc[end] != '\n') ++end;
    std::string v = doc.substr(pos, end - pos);
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    return v;
}

std::vector<double> json_numbers(const std::string& array_text) {
    std::vector<double> out;
    std::string s = array_text;
    for (char& ch : s)
        if (ch == '[' || ch == ']' || ch == ',') ch = ' ';
    std::istringstream ss(s);
    double v;
    while (ss >> v) out.push_back(v);
    return out;
}

std::vector<std::string> json_strings(const std::string& array_text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = array_text.find('"', pos)) != std::string::npos) {
        const size_t end = array_text.find('"', pos + 1);
        if (end == std::string::npos) break;
        out.push_back(array_text.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return out;
}

int32_t json_int(const std::string& doc, const std::string& key) {
    const std::string v = json_raw(doc, key);
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || x < 0 || x > INT32_MAX) throw std::runtime_error("bad integer for " + key + ": " + v);
    return static_cast<int32_t>(x);
}

}  // namespace

Raster read_raster(const std::string& dir, const std::string& name) {
    const std::string base = dir + "/raster_" + name;
    const std::string doc = slurp(base + ".json");
    Raster r;
    r.name = name;
    r.width = json_int(doc, "width");
    r.height = json_int(doc, "height");
    if (r.width <= 0 || r.height <= 0) throw std::runtime_error("non-positive raster size: " + base);
    r.dtype = json_raw(doc, "dtype");
    const auto t = json_numbers(json_raw(doc, "transform"));
    if (t.size() != 4) throw std::runtime_error("transform must have 4 numbers: " + base);
    r.transform = fz_transform{t[0], t[1], t[2], t[3]};
    const std::string nd = json_raw(doc, "nodata");
    r.has_nodata = nd != "null";
    r.nodata = r.has_nodata ? std::strtod(nd.c_str(), nullptr) : 0.0;
    const size_t n = static_cast<size_t>(r.width) * static_cast<size_t>(r.height);
    r.pixels64.resize(n);
    // NoData is compared in the storage type, as the C# harness does, so a float32 sentinel
    // matches float32 samples exactly.
    if (r.dtype == "f32") {
        const auto raw = read_binary<float>(base + ".bin");
        if (raw.size() != n) throw std::runtime_error("size mismatch: " + base);
        const float nd32 = static_cast<float>(r.nodata);
        for (size_t i = 0; i < n; ++i) r.pixels64[i] = (r.has_nodata && raw[i] == nd32) ? -9999.0 : raw[i];
    } else if (r.dtype == "f64") {
        r.pixels64 = read_binary<double>(base + ".bin");
        if (r.pixels64.size() != n) throw std::runtime_error("size mismatch: " + base);
        if (r.has_nodata)
            for (double& v : r.pixels64)
                if (v == r.nodata) v = -9999.0;
    } else if (r.dtype == "i32") {
        const auto raw = read_binary<int32_t>(base + ".bin");
        if (raw.size() != n) throw std::runtime_error("size mismatch: " + base);
        const double ndv = r.nodata;
        for (size_t i = 0; i < n; ++i) {
            const bool is_nd = r.has_nodata && static_cast<double>(raw[i]) == ndv;
            r.pixels64[i] = is_nd ? -9999.0 : static_cast<double>(raw[i]);
        }
    } else {
        throw std::runtime_error("unsupported dtype " + r.dtype);
    }
    r.pixels32.resize(n);
    for (size_t i = 0; i < n; ++i) r.pixels32[i] = static_cast<float>(r.pixels64[i]);
    return r;
}

Zones read_zones(const std::string& dir, const std::string& name) {
    const std::string base = dir + "/zones_" + name;
    const std::string doc = slurp(base + ".json");
    Zones z;
    z.name = name;
    z.n_zones = json_int(doc, "n_zones");
    z.xy = read_binary<double>(base + ".xy.f64");
    z.ring_start = read_binary<int64_t>(base + ".ring_start.i64");
    z.ring_is_hole = read_binary<uint8_t>(base + ".ring_is_hole.u8");
    z.ring_zone = read_binary<int32_t>(base + ".ring_zone.i32");
    const size_t n_rings = z.ring_zone.size();
    if (z.ring_start.size() != n_rings + 1 || z.ring_is_hole.size() != n_rings)
        throw std::runtime_error("ring arrays disagree: " + base);
    const int64_t n_points = static_cast<int64_t>(z.xy.size() / 2);
    if (z.ring_start[0] < 0) throw std::runtime_error("negative ring offset: " + base);
    for (size_t i = 0; i < n_rings; ++i) {
        if (z.ring_start[i + 1] < z.ring_start[i] || z.ring_start[i + 1] > n_points)
            throw std::runtime_error("ring offsets outside the coordinate array: " + base);
        if (z.ring_zone[i] < 0 || z.ring_zone[i] >= z.n_zones)
            throw std::runtime_error("ring zone outside n_zones: " + base);
    }
    return z;
}

Centroids read_centroids(const std::string& dir, const std::string& zones, const std::string& raster,
                         int32_t n_zones) {
    Centroids c;
    const std::string path = dir + "/zones_" + zones + ".centroid_rc_" + raster + ".i32";
    std::ifstream probe(path, std::ios::binary);
    if (!probe) return c;
    probe.close();
    const auto rc = read_binary<int32_t>(path);
    if (rc.size() != 2 * static_cast<size_t>(n_zones))
        throw std::runtime_error("centroid file does not hold n_zones pairs: " + path);
    c.rows.resize(static_cast<size_t>(n_zones));
    c.cols.resize(static_cast<size_t>(n_zones));
    for (size_t z = 0; z < c.rows.size(); ++z) {
        c.rows[z] = rc[2 * z];
        c.cols[z] = rc[2 * z + 1];
    }
    return c;
}

FixtureMeta read_fixture_meta(const std::string& dir) {
    const std::string doc = slurp(dir + "/fixture.json");
    FixtureMeta m;
    m.id = json_raw(doc, "id");
    m.rasters = json_strings(json_raw(doc, "rasters"));
    m.zones = json_strings(json_raw(doc, "zones"));
    return m;
}

std::vector<double> read_expected(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<double> out;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        const size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        out.push_back(std::strtod(line.c_str() + comma + 1, nullptr));
    }
    return out;
}

std::vector<double> resample_nearest(const Raster& src, const Raster& target) {
    std::vector<double> out(static_cast<size_t>(target.width) * target.height);
    for (int32_t r = 0; r < target.height; ++r) {
        const double y = target.transform.origin_y - (r + 0.5) * target.transform.pixel_h;
        const int64_t sr = static_cast<int64_t>(std::floor((src.transform.origin_y - y) / src.transform.pixel_h));
        for (int32_t c = 0; c < target.width; ++c) {
            const double x = target.transform.origin_x + (c + 0.5) * target.transform.pixel_w;
            const int64_t sc = static_cast<int64_t>(std::floor((x - src.transform.origin_x) / src.transform.pixel_w));
            const bool outside = sr < 0 || sr >= src.height || sc < 0 || sc >= src.width;
            out[static_cast<size_t>(r) * target.width + c] =
                outside ? -9999.0 : src.pixels64[static_cast<size_t>(sr) * src.width + static_cast<size_t>(sc)];
        }
    }
    return out;
}

}  // namespace fzio
