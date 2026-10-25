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
    if (bytes % static_cast<std::streamsize>(sizeof(T)) != 0) throw std::runtime_error("odd size: " + path);
    std::vector<T> out(static_cast<size_t>(bytes / static_cast<std::streamsize>(sizeof(T))));
    in.seekg(0);
    if (!out.empty()) in.read(reinterpret_cast<char*>(out.data()), bytes);
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
        return doc.substr(pos + 1, end - pos - 1);
    }
    if (doc[pos] == '[') {
        const size_t end = doc.find(']', pos);
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

}  // namespace

Raster read_raster(const std::string& dir, const std::string& name) {
    const std::string base = dir + "/raster_" + name;
    const std::string doc = slurp(base + ".json");
    Raster r;
    r.name = name;
    r.width = static_cast<int32_t>(std::strtol(json_raw(doc, "width").c_str(), nullptr, 10));
    r.height = static_cast<int32_t>(std::strtol(json_raw(doc, "height").c_str(), nullptr, 10));
    r.dtype = json_raw(doc, "dtype");
    const auto t = json_numbers(json_raw(doc, "transform"));
    if (t.size() != 4) throw std::runtime_error("transform must have 4 numbers: " + base);
    r.transform = fz_transform{t[0], t[1], t[2], t[3]};
    const std::string nd = json_raw(doc, "nodata");
    r.has_nodata = nd != "null";
    r.nodata = r.has_nodata ? std::strtod(nd.c_str(), nullptr) : 0.0;
    const size_t n = static_cast<size_t>(r.width) * static_cast<size_t>(r.height);
    r.pixels64.resize(n);
    if (r.dtype == "f32") {
        const auto raw = read_binary<float>(base + ".bin");
        if (raw.size() != n) throw std::runtime_error("size mismatch: " + base);
        for (size_t i = 0; i < n; ++i) r.pixels64[i] = raw[i];
    } else if (r.dtype == "f64") {
        r.pixels64 = read_binary<double>(base + ".bin");
        if (r.pixels64.size() != n) throw std::runtime_error("size mismatch: " + base);
    } else if (r.dtype == "i32") {
        const auto raw = read_binary<int32_t>(base + ".bin");
        if (raw.size() != n) throw std::runtime_error("size mismatch: " + base);
        for (size_t i = 0; i < n; ++i) r.pixels64[i] = raw[i];
    } else {
        throw std::runtime_error("unsupported dtype " + r.dtype);
    }
    if (r.has_nodata)
        for (double& v : r.pixels64)
            if (v == r.nodata) v = -9999.0;
    r.pixels32.resize(n);
    for (size_t i = 0; i < n; ++i) r.pixels32[i] = static_cast<float>(r.pixels64[i]);
    return r;
}

Zones read_zones(const std::string& dir, const std::string& name) {
    const std::string base = dir + "/zones_" + name;
    const std::string doc = slurp(base + ".json");
    Zones z;
    z.name = name;
    z.n_zones = static_cast<int32_t>(std::strtol(json_raw(doc, "n_zones").c_str(), nullptr, 10));
    z.xy = read_binary<double>(base + ".xy.f64");
    z.ring_start = read_binary<int64_t>(base + ".ring_start.i64");
    z.ring_is_hole = read_binary<uint8_t>(base + ".ring_is_hole.u8");
    z.ring_zone = read_binary<int32_t>(base + ".ring_zone.i32");
    if (z.ring_start.size() != z.ring_zone.size() + 1) throw std::runtime_error("ring arrays disagree: " + base);
    return z;
}

Centroids read_centroids(const std::string& dir, const std::string& zones, const std::string& raster) {
    Centroids c;
    const std::string path = dir + "/zones_" + zones + ".centroid_rc_" + raster + ".i32";
    std::ifstream probe(path, std::ios::binary);
    if (!probe) return c;
    probe.close();
    const auto rc = read_binary<int32_t>(path);
    c.rows.resize(rc.size() / 2);
    c.cols.resize(rc.size() / 2);
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
