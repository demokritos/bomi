#include "audioanalyzer.hpp"
#include "misc/log.hpp"
#include "tmp/algorithm.hpp"

// calculate dynamic audio normalization
// basic idea and some codes are taken from DynamicAudioNormalizer
// https://github.com/lordmulder/DynamicAudioNormalizer

DECLARE_LOG_CONTEXT(Audio)

class AudioFrameChunk {
public:
    AudioFrameChunk() { }
    AudioFrameChunk(const AudioBufferFormat &format, const AudioFilter *filter, int frames)
        : m_format(format), m_filter(filter), m_frames(frames) { }
    auto push(AudioBufferPtr buffer) -> AudioBufferPtr
    {
        Q_ASSERT(m_filter);
        if (isFull() || buffer->isEmpty())
            return buffer;
        Q_ASSERT(m_format.channels().num == buffer->channels());
        const int space = m_frames - m_filled;
        if (space >= buffer->frames()) {
            m_filled += buffer->frames();
            d.push_back(std::move(buffer));
            return AudioBufferPtr();
        }
        auto view = buffer->constView<float>();
        auto head = m_filter->newBuffer(m_format, space);
        auto tail = m_filter->newBuffer(m_format, buffer->frames() - space);
        memcpy(head->data()[0], view.begin(), head->samples() * sizeof(float));
        memcpy(tail->data()[0], view.begin() + head->samples(), tail->samples() * sizeof(float));
        m_filled += head->frames();
        d.push_back(std::move(head));
        Q_ASSERT(m_frames == m_filled);
        return tail;
    }
    auto pop() -> AudioBufferPtr
    {
        auto ret = tmp::take_front(d);
        if (ret)
            m_filled -= ret->frames();
        return ret;
    }
    auto samples() const -> int { return m_frames * m_format.channels().num; }
    auto filled() const -> int { return m_filled; }
    auto frames() const -> int { return m_frames; }
    auto isFull() const -> bool { return m_filled >= m_frames; }
    auto max(bool *silence) const -> double
    {
        double max = 0.0, avg = 0.0;
        for (auto &buffer : d) {
            auto view = buffer->constView<float>();
            for (float v : view) {
                const auto a = qAbs(v);
                max = std::max<double>(a, max);
                avg += a;
            }
        }
        avg /= m_filled * m_format.channels().num;
        *silence = avg < 1e-4;
        return max;
    }
    auto rms() const -> double
    {
        double sum2 = 0.0;
        for (auto &buffer : d) {
            auto view = buffer->constView<float>();
            for (float v : view)
                sum2 += v * v;
        }
        return sqrt(sum2 / (m_filled * m_format.channels().num));
    }
private:
    AudioBufferFormat m_format;
    std::deque<AudioBufferPtr> d;
    const AudioFilter *m_filter = nullptr;
    int m_filled = 0, m_frames = 0;
};

struct AudioAnalyzer::Data {
    AudioAnalyzer *p = nullptr;
    AudioBufferFormat format;
    AudioNormalizerOption option;
    int frames = 0, gaussian_r = 0;
    double scale = 1.0;
    bool normalizer = false;
    struct {
        std::deque<double> orig, min, smooth;
        double prev = 1.0, current = 1.0;
        auto clear() { prev = current = 1.0; orig.clear(); min.clear(); smooth.clear(); }
    } history;
    std::deque<AudioFrameChunk> inputs, outputs;
    AudioFrameChunk filling;
    std::vector<double> gaussian_w;

    auto chunk() const -> AudioFrameChunk { return { format, p, frames }; }
    auto gaussian(const std::deque<double> &data) const -> double
    {
        Q_ASSERT(data.size() == gaussian_w.size());
        int i = 0; double ret = 0.0;
        for (auto v : data)
            ret += v * gaussian_w[i++];
        return ret;
    }

    auto update(float gain) -> void
    {
        if((int)history.orig.size() < gaussian_r) {
            history.current = history.prev = gain;
            history.orig.insert(history.orig.end(), gaussian_r, history.prev);
            history.min.insert(history.min.end(), gaussian_r, history.prev);
        }
        history.orig.push_back(gain);
#define PUSH_POP(from, to, filter) while(from.size() >= gaussian_w.size()) \
            { to.push_back(filter(from)); from.pop_front(); }
        PUSH_POP(history.orig, history.min, tmp::min);
        PUSH_POP(history.min, history.smooth, gaussian);
#undef PUSH_POP
    }
};

AudioAnalyzer::AudioAnalyzer()
    : d(new Data)
{
    d->p = this;
}

AudioAnalyzer::~AudioAnalyzer()
{
    delete d;
}

auto AudioAnalyzer::setFormat(const AudioBufferFormat &format) -> void
{
    if (!_Change(d->format, format))
        return;
    d->format = format;
    reset();
    d->history.clear();
}

auto AudioAnalyzer::reset() -> void
{
    d->inputs.clear();
    d->outputs.clear();
    d->frames = d->format.secToFrames(d->option.chunk_sec);
    d->filling = d->chunk();
}

auto AudioAnalyzer::isNormalizerActive() const -> bool
{
    return d->normalizer;
}

auto AudioAnalyzer::setNormalizerActive(bool on) -> void
{
    if (_Change(d->normalizer, on))
        d->history.current = 1.0;
}

auto AudioAnalyzer::gain() const -> float
{
    return d->history.current;
}

auto AudioAnalyzer::setScale(double scale) -> void
{
    d->scale = scale;
}

auto AudioAnalyzer::delay() const -> double
{
    int frames = d->filling.filled();
    for (const auto &chunk : d->inputs)
        frames += chunk.filled();
    for (const auto &chunk : d->outputs)
        frames += chunk.filled();
    return d->format.toSeconds(frames) / d->scale;
}

auto AudioAnalyzer::setNormalizerOption(const AudioNormalizerOption &opt) -> void
{
    d->option.use_rms   = opt.use_rms;
    d->option.smoothing = std::max(1, opt.smoothing);
    d->option.chunk_sec = qBound(0.1, opt.chunk_sec, 1.0);
    d->option.max       = std::min(10.0, opt.max);
    d->option.target    = std::min(0.95, opt.target);

    const int radius = d->option.smoothing;
    const int size = radius * 2 + 1;
    const int shift = radius;
    if ((int)d->gaussian_w.size() == size)
        return;
    d->gaussian_r = radius;
    d->gaussian_w.resize(size);

    const double sigma = radius / 3.0;
    const double c = 2.0 * pow(sigma, 2);
    auto func = [&] (int i) { return exp(-(pow(i - shift, 2) / c)); };
    double sum = 0.0;
    for(int i = 0; i < size; ++i)
        sum += (d->gaussian_w[i] = func(i));
    for(int i = 0; i < size; ++i)
        d->gaussian_w[i] /= sum; // normalize

    reset();
    d->history.clear();
}

auto AudioAnalyzer::passthrough(const AudioBufferPtr &) const -> bool
{
    return false;
}

auto AudioAnalyzer::push(AudioBufferPtr &src) -> void
{
    Q_ASSERT(!d->filling.isFull());
    AudioBufferPtr left = std::move(src);
    while (left && !left->isEmpty()) {
        left = d->filling.push(left);
        if (d->filling.isFull()) {
            d->inputs.push_back(std::move(d->filling));
            d->filling = d->chunk();
        }
    }
}

static auto cutoff(double value, double cutoff)
{
    constexpr double c = 0.8862269254527580136490837416; //~ sqrt(PI) / 2.0
    return erf(c * (value / cutoff)) * cutoff;
}

auto AudioAnalyzer::pull(bool eof) -> AudioBufferPtr
{
    if (!d->normalizer)
        return flush();
    while (!d->inputs.empty()) {
        auto chunk = tmp::take_front(d->inputs);
        double gain = d->history.prev;
        bool silence = false;
        const auto max = chunk.max(&silence);
        if (!silence) {
            if (d->option.use_rms) {
                const double peak = 0.95 / max;
                const double rms = d->option.target / chunk.rms();
                gain = std::min(peak, rms);
            } else
                gain = d->option.target / max;
        }
        d->update(cutoff(gain, d->option.max));
        d->outputs.push_back(std::move(chunk));
    }
    if (d->history.smooth.empty())
        return eof ? flush() : AudioBufferPtr();
    Q_ASSERT(!d->outputs.empty());
    auto &chunk = d->outputs.front();
    const auto r = chunk.filled() / double(chunk.frames());
    d->history.current = d->history.prev * r + (1.0 - r) * d->history.smooth.front();
    auto buffer = d->outputs.front().pop();
    if (!buffer) {
        d->outputs.pop_front();
        d->history.prev = tmp::take_front(d->history.smooth);
    }
    return buffer;
}

auto AudioAnalyzer::flush() -> AudioBufferPtr
{
    auto pop_deque = [&] (auto &d) {
        auto ret = d.front().pop();
        if (!ret) d.pop_front();
        return ret;
    };
    if (!d->outputs.empty())
        return pop_deque(d->outputs);
    if (!d->inputs.empty())
        return pop_deque(d->inputs);
    return d->filling.pop();
}

auto AudioAnalyzer::run(AudioBufferPtr &in) -> AudioBufferPtr
{
    return in;
}
