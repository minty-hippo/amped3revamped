// amped3-subtitles: who says each line (speakers per clip, up to four).
//
// Each subtitle line's audio becomes a voice embedding with Resemblyzer's
// voice encoder (Apache 2.0, resemble-ai/Resemblyzer; GE2E, 3-layer LSTM of
// 256 over 40 mel bands, 1.6 s partials): the installer downloads its
// pretrained.pt (17 MB) at a pinned commit and checks its hash, and the
// weights are read straight out of it here (the file's layout is fixed by
// that hash: the LSTM's flat weights and the linear layer at known places).
// The lines of a clip are then grouped by how alike their voices are
// (average-linkage clustering of cosine distances, at most four groups) and
// numbered in the order they first speak: speaker 1 white, 2 yellow, 3 pink,
// 4 green on screen.
//
// Checked on the game's own riders (29 voices, labelled in its sound
// project): lines of a few seconds land with the right speaker ~85% of the
// time; one-second shouts are harder. The subtitle editor fixes the rest.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace amped3::speakers {

// pretrained.pt at Resemblyzer commit 15d828edebe06bc72b9cabc8ef8ca5ab2cb457ce.
inline constexpr const char* kModelSha256 = "39373b86598fa3da9fcddee6142382efe09777e8d37dc9c0561f41f0070f134e";
inline constexpr size_t kModelSize = 17090379;
inline constexpr double kPi = 3.14159265358979323846;  // (M_PI isn't standard: not on Windows)

class VoiceEncoder {
 public:
  bool Load(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
      *error = "can't open " + path;
      return false;
    }
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() != kModelSize) {
      *error = "not the expected voice encoder model (size)";
      return false;
    }
    // Storages: an int64 count, then the floats. The LSTM's flat weights
    // (1357824 floats), the linear layer's weight (65536) and bias (256).
    constexpr size_t kFlat = 269883, kLinW = 7731, kLinB = 6699;
    auto count_at = [&](size_t data) {
      int64_t n = 0;
      std::memcpy(&n, &d[data - 8], 8);
      return n;
    };
    if (count_at(kFlat) != 1357824 || count_at(kLinW) != 65536 || count_at(kLinB) != 256) {
      *error = "not the expected voice encoder model (layout)";
      return false;
    }
    auto floats = [&](size_t byte, size_t n) {
      std::vector<float> v(n);
      std::memcpy(v.data(), &d[byte], n * 4);  // little-endian on every platform built for
      return v;
    };
    // Offsets (floats) into the flat LSTM weights: w_ih, w_hh, b_ih, b_hh.
    const size_t offs[3][4] = {{0, 40960, 1351680, 1352704},
                               {303104, 565248, 1353728, 1354752},
                               {827392, 1089536, 1355776, 1356800}};
    for (int l = 0; l < 3; ++l) {
      const size_t in = l == 0 ? 40 : 256;
      Layer& L = layers_[l];
      L.in = in;
      L.wih = floats(kFlat + offs[l][0] * 4, 1024 * in);
      L.whh = floats(kFlat + offs[l][1] * 4, 1024 * 256);
      L.b = floats(kFlat + offs[l][2] * 4, 1024);
      const auto bhh = floats(kFlat + offs[l][3] * 4, 1024);
      for (int i = 0; i < 1024; ++i) L.b[i] += bhh[i];
    }
    lin_w_ = floats(kLinW, 65536);
    lin_b_ = floats(kLinB, 256);
    MakeMel();
    loaded_ = true;
    return true;
  }
  bool loaded() const { return loaded_; }

  // A 256-value voice embedding (unit length) of 16 kHz mono audio.
  std::vector<float> Embed(std::vector<float> wav) const {
    // Louder to -30 dBFS (never quieter), as the model was trained.
    double sum = 0;
    for (float v : wav) sum += double(v) * v;
    const double rms = std::sqrt(sum / std::max<size_t>(1, wav.size())) + 1e-9;
    const double gain_db = -30.0 - 20.0 * std::log10(rms);
    if (gain_db > 0) {
      const float g = float(std::pow(10.0, gain_db / 20.0));
      for (float& v : wav) v *= g;
    }
    // Partials of 160 frames, 1.3 a second; the last kept when it's 75% there.
    const int n_frames = int(std::ceil((wav.size() + 1) / 160.0));
    const int step = int(std::lround((16000.0 / 1.3) / 160.0));
    const int steps = std::max(1, n_frames - 160 + step + 1);
    std::vector<int> starts;
    for (int i = 0; i < steps; i += step) starts.push_back(i);
    const double coverage = (double(wav.size()) - starts.back() * 160.0) / (160.0 * 160.0);
    if (coverage < 0.75 && starts.size() > 1) starts.pop_back();
    const size_t need = size_t(starts.back() + 160) * 160;
    if (need >= wav.size()) wav.resize(need, 0.0f);
    const std::vector<float> mel = Mel(wav);
    const int frames = int(mel.size() / 40);
    std::vector<float> sum_e(256, 0.0f);
    for (int s : starts) {
      std::vector<float> e = Partial(mel.data() + size_t(s) * 40, std::min(160, frames - s));
      for (int i = 0; i < 256; ++i) sum_e[i] += e[i];
    }
    Normalize(sum_e);
    return sum_e;
  }

 private:
  struct Layer {
    size_t in = 0;
    std::vector<float> wih, whh, b;
  };

  static void Normalize(std::vector<float>& v) {
    double n = 0;
    for (float x : v) n += double(x) * x;
    n = std::sqrt(n);
    if (n > 0) {
      for (float& x : v) x = float(x / n);
    }
  }

  // Slaney mel filters (as librosa's), 40 bands, 400-point FFT at 16 kHz.
  void MakeMel() {
    auto hz2mel = [](double f) {
      const double fs = 200.0 / 3, lo = 1000, lm = lo / fs, ls = std::log(6.4) / 27;
      return f >= lo ? lm + std::log(f / lo) / ls : f / fs;
    };
    auto mel2hz = [](double m) {
      const double fs = 200.0 / 3, lo = 1000, lm = lo / fs, ls = std::log(6.4) / 27;
      return m >= lm ? lo * std::exp(ls * (m - lm)) : fs * m;
    };
    double pts[42];
    const double top = hz2mel(8000);
    for (int i = 0; i < 42; ++i) pts[i] = mel2hz(top * i / 41.0);
    fb_.assign(40 * 201, 0.0f);
    for (int m = 0; m < 40; ++m) {
      const double enorm = 2.0 / (pts[m + 2] - pts[m]);
      for (int k = 0; k < 201; ++k) {
        const double f = 8000.0 * k / 200.0;
        const double lower = (f - pts[m]) / (pts[m + 1] - pts[m]);
        const double upper = (pts[m + 2] - f) / (pts[m + 2] - pts[m + 1]);
        fb_[m * 201 + k] = float(std::max(0.0, std::min(lower, upper)) * enorm);
      }
    }
    for (int n = 0; n < 400; ++n) window_[n] = float(0.5 - 0.5 * std::cos(2 * kPi * n / 400.0));
    for (int k = 0; k < 201; ++k) {
      for (int n = 0; n < 400; ++n) {
        cos_[k * 400 + n] = float(std::cos(2 * kPi * k * n / 400.0));
        sin_[k * 400 + n] = float(std::sin(2 * kPi * k * n / 400.0));
      }
    }
  }

  // Power mel spectrogram (not log), centred frames (zero padded), hop 160.
  std::vector<float> Mel(const std::vector<float>& wav) const {
    const size_t n = 1 + wav.size() / 160;
    std::vector<float> out(n * 40);
    float frame[400], power[201];
    for (size_t t = 0; t < n; ++t) {
      for (int i = 0; i < 400; ++i) {
        const long at = long(t * 160) + i - 200;
        frame[i] = (at >= 0 && size_t(at) < wav.size() ? wav[size_t(at)] : 0.0f) * window_[i];
      }
      for (int k = 0; k < 201; ++k) {
        float re = 0, im = 0;
        const float* c = &cos_[k * 400];
        const float* s = &sin_[k * 400];
        for (int i = 0; i < 400; ++i) {
          re += frame[i] * c[i];
          im += frame[i] * s[i];
        }
        power[k] = re * re + im * im;
      }
      for (int m = 0; m < 40; ++m) {
        float acc = 0;
        const float* w = &fb_[m * 201];
        for (int k = 0; k < 201; ++k) acc += w[k] * power[k];
        out[t * 40 + m] = acc;
      }
    }
    return out;
  }

  static float Sigmoid(float x) { return 1.0f / (1.0f + std::exp(-std::clamp(x, -30.0f, 30.0f))); }

  // The embedding of one partial: the LSTM's last hidden state, a linear
  // layer, ReLU, unit length.
  std::vector<float> Partial(const float* mel, int frames) const {
    std::vector<float> in(mel, mel + size_t(frames) * 40), out;
    std::vector<float> h(256), c(256), g(1024);
    for (const Layer& L : layers_) {
      out.assign(size_t(frames) * 256, 0.0f);
      std::fill(h.begin(), h.end(), 0.0f);
      std::fill(c.begin(), c.end(), 0.0f);
      for (int t = 0; t < frames; ++t) {
        const float* x = &in[size_t(t) * L.in];
        for (int r = 0; r < 1024; ++r) {
          float acc = L.b[r];
          const float* wi = &L.wih[size_t(r) * L.in];
          for (size_t i = 0; i < L.in; ++i) acc += wi[i] * x[i];
          const float* wh = &L.whh[size_t(r) * 256];
          for (int i = 0; i < 256; ++i) acc += wh[i] * h[i];
          g[r] = acc;
        }
        for (int i = 0; i < 256; ++i) {
          const float ig = Sigmoid(g[i]), fg = Sigmoid(g[256 + i]);
          const float gg = std::tanh(g[512 + i]), og = Sigmoid(g[768 + i]);
          c[i] = fg * c[i] + ig * gg;
          h[i] = og * std::tanh(c[i]);
        }
        std::copy(h.begin(), h.end(), out.begin() + size_t(t) * 256);
      }
      in.swap(out);
    }
    std::vector<float> e(256);
    for (int r = 0; r < 256; ++r) {
      float acc = lin_b_[r];
      for (int i = 0; i < 256; ++i) acc += lin_w_[size_t(r) * 256 + i] * h[i];
      e[r] = std::max(0.0f, acc);
    }
    Normalize(e);
    return e;
  }

  bool loaded_ = false;
  Layer layers_[3];
  std::vector<float> lin_w_, lin_b_;
  std::vector<float> fb_;
  float window_[400];
  std::vector<float> cos_ = std::vector<float>(201 * 400), sin_ = std::vector<float>(201 * 400);
};

// Groups embeddings (one per line; empty = no voice to judge, joins the
// speaker before it) into at most four speakers, numbered by first line.
inline std::vector<int> Group(const std::vector<std::vector<float>>& e, double threshold = 0.30) {
  const size_t n = e.size();
  std::vector<int> idx;
  for (size_t i = 0; i < n; ++i) {
    if (!e[i].empty()) idx.push_back(int(i));
  }
  std::vector<int> label(n, 0);
  if (idx.empty()) return label;
  const size_t m = idx.size();
  std::vector<double> dist(m * m, 0.0);
  for (size_t a = 0; a < m; ++a) {
    for (size_t b = a + 1; b < m; ++b) {
      double dot = 0;
      for (size_t k = 0; k < e[size_t(idx[a])].size(); ++k) dot += double(e[size_t(idx[a])][k]) * e[size_t(idx[b])][k];
      dist[a * m + b] = dist[b * m + a] = 1.0 - dot;
    }
  }
  std::vector<std::vector<size_t>> groups;
  for (size_t a = 0; a < m; ++a) groups.push_back({a});
  while (groups.size() > 1) {
    double best = 1e9;
    size_t bi = 0, bj = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
      for (size_t j = i + 1; j < groups.size(); ++j) {
        double s = 0;
        for (size_t x : groups[i]) {
          for (size_t y : groups[j]) s += dist[x * m + y];
        }
        s /= double(groups[i].size() * groups[j].size());
        if (s < best) {
          best = s;
          bi = i;
          bj = j;
        }
      }
    }
    if (best > threshold && groups.size() <= 4) break;
    groups[bi].insert(groups[bi].end(), groups[bj].begin(), groups[bj].end());
    groups.erase(groups.begin() + std::ptrdiff_t(bj));
  }
  std::vector<int> group_of(m, 0);
  for (size_t g = 0; g < groups.size(); ++g) {
    for (size_t x : groups[g]) group_of[x] = int(g);
  }
  // Numbered by who speaks first.
  std::vector<int> order(groups.size(), -1);
  int next = 0;
  for (size_t a = 0; a < m; ++a) {
    int& o = order[size_t(group_of[a])];
    if (o < 0) o = next++;
    label[size_t(idx[a])] = o;
  }
  for (size_t i = 1; i < n; ++i) {
    if (e[i].empty()) label[i] = label[i - 1];
  }
  return label;
}

}  // namespace amped3::speakers
