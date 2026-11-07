#include "dsptb/core/export_job.hpp"

#include "dsptb/core/paths.hpp"

#include <algorithm>

namespace dsptb {

namespace {
// The part of `input` the export covers, as a new buffer.
AudioBuffer slice(const AudioBuffer& in, const ExportOptions& o) {
    if (!o.loopRegionOnly)
        return in;
    const double sr = static_cast<double>(in.sampleRate);
    const size_t a = std::min(in.frames(), static_cast<size_t>(std::max(0.0, o.regionStartSeconds) * sr));
    const size_t b = std::clamp(static_cast<size_t>(std::max(0.0, o.regionEndSeconds) * sr), a, in.frames());
    AudioBuffer out;
    out.sampleRate = in.sampleRate;
    for (int c = 0; c < 2; ++c)
        out.ch[c].assign(in.ch[c].begin() + static_cast<std::ptrdiff_t>(a),
                         in.ch[c].begin() + static_cast<std::ptrdiff_t>(b));
    return out;
}
} // namespace

std::shared_ptr<ExportJob> ExportJob::start(WorkerPool& pool, ModuleRegistry& registry,
                                            const SessionDescription& session, const AudioBuffer& input,
                                            const ExportOptions& options, std::string& error) {
    if (input.frames() == 0) {
        error = "there is no audio to export";
        return nullptr;
    }
    for (const InstanceDescription& d : session.instances) {
        ModulePackage* pkg = nullptr;
        if (!registry.descriptor(d.module, &pkg, error))
            return nullptr; // loads the DLL now, on this thread
    }
    std::shared_ptr<ExportJob> job(new ExportJob);
    job->registry_ = &registry;
    job->session_ = session;
    job->options_ = options;
    job->options_.tailSeconds = std::clamp(options.tailSeconds, 0.0, 10.0);
    job->input_ = slice(input, options);
    if (job->input_.frames() == 0) {
        error = "the loop region is empty";
        return nullptr;
    }
    pool.submit([job] { job->run(); });
    return job;
}

// Runs on a worker thread and uses the (unlocked) ModuleRegistry. Safe today because the export dialog is
// modal and the UI thread does not touch the registry while a job runs; a rescan or hot reload would need a lock.
void ExportJob::run() {
    RenderOptions ro;
    ro.blockFrames = options_.blockFrames;
    ro.tailSeconds = options_.tailSeconds;
    ro.progress = &progress_;
    ro.cancel = &cancel_;
    AudioBuffer rendered;
    std::string error;
    bool ok = renderOffline(*registry_, session_, input_, ro, rendered, error);
    cancelled_ = !ok && error == "cancelled";

    std::filesystem::path part = options_.out;
    part += ".part";
    if (ok) {
        PcmAudio pcm;
        pcm.channels = 2;
        pcm.sampleRate = rendered.sampleRate;
        pcm.samples.resize(rendered.frames() * 2);
        for (size_t i = 0; i < rendered.frames(); ++i) {
            pcm.samples[2 * i] = rendered.ch[0][i];
            pcm.samples[2 * i + 1] = rendered.ch[1][i];
        }
        // 16-bit gets TPDF dither; the others never do.
        ok = writeWav(part, pcm, options_.format, error, options_.format == WavFormat::S16);
        if (ok && cancel_.load()) { // cancelled while writing
            ok = false;
            cancelled_ = true;
            error = "cancelled";
        }
    }
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(part, options_.out, ec); // replaces an existing file on Windows
        if (ec) {
            ok = false;
            error = "cannot write " + toUtf8(options_.out) + ": " + ec.message();
        }
    }
    if (!ok)
        std::filesystem::remove(part, ec);
    ok_ = ok;
    error_ = cancelled_ ? std::string() : error;
    done_.store(true, std::memory_order_release);
}

} // namespace dsptb
