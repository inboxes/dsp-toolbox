// dsptb-sofa: developer tool for preparing HRTF sets (not shipped). Reads SOFA files with libmysofa.
//   dsptb-sofa info <file.sofa>

#include <mysofa.h>
#include <netcdf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace {

void printAttributes(const char* title, const struct MYSOFA_ATTRIBUTE* a) {
    std::printf("%s\n", title);
    for (; a; a = a->next)
        std::printf("  %s = %s\n", a->name ? a->name : "?", a->value ? a->value : "?");
}

int info(const char* path) {
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load(path, &err);
    if (!h) {
        std::printf("mysofa_load failed: %d\n", err);
        return 1;
    }
    std::printf("measurements M=%u receivers R=%u samples N=%u emitters E=%u\n", h->M, h->R, h->N, h->E);
    printAttributes("global attributes:", h->attributes);
    printAttributes("SourcePosition attributes:", h->SourcePosition.attributes);
    printAttributes("Data.IR attributes:", h->DataIR.attributes);
    printAttributes("Data.SamplingRate attributes:", h->DataSamplingRate.attributes);
    printAttributes("Data.Delay attributes:", h->DataDelay.attributes);
    if (h->DataSamplingRate.elements)
        std::printf("sampling rate: %g Hz\n", static_cast<double>(h->DataSamplingRate.values[0]));
    std::printf("delay values: %u (first %g)\n", h->DataDelay.elements,
                h->DataDelay.elements ? static_cast<double>(h->DataDelay.values[0]) : 0.0);
    std::printf("listener position values: %u\n", h->ListenerPosition.elements);

    // SourcePosition is (azimuth deg, elevation deg, radius m) for the spherical type, else (x, y, z).
    const float* p = h->SourcePosition.values;
    std::map<int, unsigned> radii; // in mm
    float minEl = 1e9f, maxEl = -1e9f;
    for (unsigned m = 0; m < h->M; ++m) {
        const float* v = p + m * 3;
        float az = v[0], el = v[1], r = v[2];
        // Convert to spherical when the file stores Cartesian coordinates.
        const char* type = nullptr;
        for (const MYSOFA_ATTRIBUTE* a = h->SourcePosition.attributes; a; a = a->next)
            if (a->name && std::strcmp(a->name, "Type") == 0)
                type = a->value;
        if (type && std::strcmp(type, "cartesian") == 0) {
            r = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            el = std::asin(v[2] / std::max(r, 1e-9f)) * 57.29578f;
            az = std::atan2(v[1], v[0]) * 57.29578f;
        }
        (void)az;
        ++radii[static_cast<int>(std::lround(r * 1000.0f))];
        minEl = std::min(minEl, el);
        maxEl = std::max(maxEl, el);
    }
    std::printf("elevation range: %.1f .. %.1f deg\n", static_cast<double>(minEl),
                static_cast<double>(maxEl));
    std::printf("distinct radii: %zu\n", radii.size());
    int shown = 0;
    for (const auto& [mm, count] : radii) {
        if (shown++ < 6 || shown == static_cast<int>(radii.size()))
            std::printf("  %.3f m: %u measurements\n", mm / 1000.0, count);
    }
    mysofa_free(h);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------
// writing: a SimpleFreeFieldHRIR file through netCDF-4, which produces the HDF5 layout SOFA readers
// (libmysofa, and so Steam Audio) expect.
// ---------------------------------------------------------------------------------------------------------

bool ncOk(int status, const char* what) {
    if (status == NC_NOERR)
        return true;
    std::printf("netCDF error in %s: %s\n", what, nc_strerror(status));
    return false;
}

void ncAttr(int ncid, int var, const char* name, const std::string& value) {
    nc_put_att_text(ncid, var, name, value.size(), value.c_str());
}

int ncVar(int ncid, const char* name, std::initializer_list<int> dims, const char* units = nullptr,
          const char* coordType = nullptr) {
    std::vector<int> d(dims);
    int id = -1;
    nc_def_var(ncid, name, NC_DOUBLE, static_cast<int>(d.size()), d.data(), &id);
    if (units)
        ncAttr(ncid, id, "Units", units);
    if (coordType)
        ncAttr(ncid, id, "Type", coordType);
    return id;
}

bool writeSofa(const std::string& path, const std::vector<float>& pos /* M x 3: az, el, radius */,
               const std::vector<double>& ir /* M x 2 x N */, unsigned n, double rate,
               const std::map<std::string, std::string>& attrs) {
    const size_t m = pos.size() / 3;
    int ncid = -1;
    if (!ncOk(nc_create(path.c_str(), NC_NETCDF4 | NC_CLOBBER, &ncid), "nc_create"))
        return false;
    ncAttr(ncid, NC_GLOBAL, "Conventions", "SOFA");
    for (const auto& [k, v] : attrs)
        ncAttr(ncid, NC_GLOBAL, k.c_str(), v);
    int dI, dC, dR, dE, dN, dM;
    nc_def_dim(ncid, "I", 1, &dI);
    nc_def_dim(ncid, "C", 3, &dC);
    nc_def_dim(ncid, "R", 2, &dR);
    nc_def_dim(ncid, "E", 1, &dE);
    nc_def_dim(ncid, "N", n, &dN);
    nc_def_dim(ncid, "M", m, &dM);
    const int vListener = ncVar(ncid, "ListenerPosition", {dI, dC}, "metre", "cartesian");
    const int vView = ncVar(ncid, "ListenerView", {dI, dC}, "metre", "cartesian");
    const int vUp = ncVar(ncid, "ListenerUp", {dI, dC}, "metre", "cartesian");
    const int vRecv = ncVar(ncid, "ReceiverPosition", {dR, dC, dI}, "metre", "cartesian");
    const int vEmit = ncVar(ncid, "EmitterPosition", {dE, dC, dI}, "metre", "cartesian");
    const int vSrc = ncVar(ncid, "SourcePosition", {dM, dC}, "degree, degree, metre", "spherical");
    const int vIr = ncVar(ncid, "Data.IR", {dM, dR, dN});
    const int vRate = ncVar(ncid, "Data.SamplingRate", {dI}, "hertz");
    const int vDelay = ncVar(ncid, "Data.Delay", {dI, dR});
    if (!ncOk(nc_enddef(ncid), "nc_enddef"))
        return false;
    const double zero3[3] = {0, 0, 0}, view[3] = {1, 0, 0}, up[3] = {0, 0, 1};
    const double ears[6] = {0, 0.09,  0,
                            0, -0.09, 0}; // (R, C, I): left ear then right ear; Steam Audio ignores them
    const double delay[2] = {0, 0};
    std::vector<double> src(pos.begin(), pos.end());
    bool ok = ncOk(nc_put_var_double(ncid, vListener, zero3), "put ListenerPosition");
    ok = ok && ncOk(nc_put_var_double(ncid, vView, view), "put ListenerView");
    ok = ok && ncOk(nc_put_var_double(ncid, vUp, up), "put ListenerUp");
    ok = ok && ncOk(nc_put_var_double(ncid, vRecv, ears), "put ReceiverPosition");
    ok = ok && ncOk(nc_put_var_double(ncid, vEmit, zero3), "put EmitterPosition");
    ok = ok && ncOk(nc_put_var_double(ncid, vSrc, src.data()), "put SourcePosition");
    ok = ok && ncOk(nc_put_var_double(ncid, vIr, ir.data()), "put Data.IR");
    ok = ok && ncOk(nc_put_var_double(ncid, vRate, &rate), "put Data.SamplingRate");
    ok = ok && ncOk(nc_put_var_double(ncid, vDelay, delay), "put Data.Delay");
    ok = ncOk(nc_close(ncid), "nc_close") && ok;
    return ok;
}

// Cuts one distance shell out of a multi-distance file (Steam Audio ignores distance, so it needs one radius
// per file).
int slice(const char* inPath, const char* outPath, double radius, const std::string& note) {
    int err = 0;
    MYSOFA_HRTF* h = mysofa_load(inPath, &err);
    if (!h) {
        std::printf("mysofa_load failed: %d\n", err);
        return 1;
    }
    std::vector<float> pos;
    std::vector<double> ir;
    float minEl = 1e9f, maxEl = -1e9f;
    double rSum = 0;
    for (unsigned m = 0; m < h->M; ++m) {
        const float* v = h->SourcePosition.values + m * 3;
        if (std::fabs(static_cast<double>(v[2]) - radius) > 0.0005)
            continue;
        pos.insert(pos.end(), {v[0], v[1], v[2]});
        rSum += v[2];
        minEl = std::min(minEl, v[1]);
        maxEl = std::max(maxEl, v[1]);
        const float* p = h->DataIR.values + static_cast<size_t>(m) * h->R * h->N;
        for (unsigned i = 0; i < h->R * h->N; ++i)
            ir.push_back(static_cast<double>(p[i]));
    }
    if (pos.empty()) {
        std::printf("no measurements at radius %.3f m\n", radius);
        mysofa_free(h);
        return 1;
    }
    std::map<std::string, std::string> attrs;
    for (const MYSOFA_ATTRIBUTE* a = h->attributes; a; a = a->next) {
        if (!a->name || !a->value || a->name[0] == '_')
            continue;
        const std::string k = a->name;
        if (k == "SOFAConventions" || k == "SOFAConventionsVersion" || k == "DataType" || k == "RoomType" ||
            k == "License" || k == "DatabaseName" || k == "Organization" || k == "AuthorContact" ||
            k == "ListenerShortName" || k == "References" || k == "Version")
            attrs[k] = a->value;
    }
    attrs["Title"] =
        "Single-distance slice of " + std::string(inPath).substr(std::string(inPath).find_last_of("/\\") + 1);
    attrs["Comment"] = note;
    attrs["History"] =
        "Cut with dsptb-sofa slice: measurements within 0.5 mm of the requested distance, copied unchanged";
    const bool ok =
        writeSofa(outPath, pos, ir, h->N, static_cast<double>(h->DataSamplingRate.values[0]), attrs);
    std::printf("%s: %zu measurements at %.3f m, elevation %.1f..%.1f deg, %u samples\n", outPath,
                pos.size() / 3, rSum / static_cast<double>(pos.size() / 3), static_cast<double>(minEl),
                static_cast<double>(maxEl), h->N);
    mysofa_free(h);
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "info") == 0)
        return info(argv[2]);
    if (argc >= 6 && std::strcmp(argv[1], "slice") == 0)
        return slice(argv[2], argv[3], std::atof(argv[4]), argv[5]);
    std::printf("usage: dsptb-sofa info <file.sofa>\n"
                "       dsptb-sofa slice <in.sofa> <out.sofa> <radius_m> <comment>\n");
    return 2;
}
