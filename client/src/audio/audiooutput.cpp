#include "audiooutput.hpp"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>

#include <string.h>
#include <sys/asoundlib.h>
#include <audio/audio_manager_routing.h>

namespace {

const int kFragMs = 20;
const int kFragBytes = AudioOutput::kBytesPerMs * kFragMs; // 3840

snd_pcm_t *openDevice(unsigned int *audioman, int *fragBytes, QString *error)
{
    snd_pcm_t *pcm = 0;
    *audioman = 0;
    // "defaultp", like mm_renderer: the routed device, which follows the
    // current output (Bluetooth A2DP, headphones, speaker) and the media
    // volume. The "preferred" PCM is the phone's own codec: on 2026-10-28 it
    // played on the speaker at 100% with a Bluetooth headset connected, deaf
    // to the volume keys.
    char name[] = "/dev/snd/defaultp";
    int rc = audio_manager_snd_pcm_open_name(AUDIO_TYPE_MULTIMEDIA, &pcm, audioman, name, SND_PCM_OPEN_PLAYBACK);
    if (rc < 0) {
        qWarning() << "[Feather:audio] defaultp:" << snd_strerror(rc) << "- trying the preferred device";
        int card = -1, device = 0;
        rc = audio_manager_snd_pcm_open_preferred(AUDIO_TYPE_MULTIMEDIA, &pcm, audioman, &card, &device,
                                                  SND_PCM_OPEN_PLAYBACK);
    }
    if (rc < 0) {
        *error = QString("open: %1").arg(snd_strerror(rc));
        return 0;
    }
    // write() transfers, not mmap (mmap had 32-sample glitches on this phone,
    // see Berry Bridge's voice recorder).
    snd_pcm_plugin_set_disable(pcm, PLUGIN_MMAP);

    snd_pcm_channel_params_t params;
    memset(&params, 0, sizeof params);
    params.mode = SND_PCM_MODE_BLOCK;
    params.channel = SND_PCM_CHANNEL_PLAYBACK;
    params.start_mode = SND_PCM_START_FULL;
    params.stop_mode = SND_PCM_STOP_STOP;
    params.buf.block.frag_size = kFragBytes;
    params.buf.block.frags_min = 1;
    params.buf.block.frags_max = 4; // the latency stays in the ring, where it is measured
    params.format.interleave = 1;
    params.format.rate = AudioOutput::kRate;
    params.format.voices = 2;
    params.format.format = SND_PCM_SFMT_S16_LE;
    strncpy(params.sw_mixer_subchn_name, "Feather", sizeof params.sw_mixer_subchn_name - 1);
    rc = snd_pcm_plugin_params(pcm, &params);
    if (rc < 0) {
        *error = QString("params: %1 (%2)").arg(snd_strerror(rc)).arg(params.why_failed);
    } else if ((rc = snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK)) < 0) {
        *error = QString("prepare: %1").arg(snd_strerror(rc));
    }
    if (!error->isEmpty()) {
        snd_pcm_close(pcm);
        if (*audioman) audio_manager_free_handle(*audioman);
        *audioman = 0;
        return 0;
    }
    snd_pcm_channel_setup_t setup;
    memset(&setup, 0, sizeof setup);
    setup.channel = SND_PCM_CHANNEL_PLAYBACK;
    snd_pcm_plugin_setup(pcm, &setup);
    *fragBytes = setup.buf.block.frag_size > 0 ? setup.buf.block.frag_size : kFragBytes;
    return pcm;
}

void closeDevice(snd_pcm_t *pcm, unsigned int audioman)
{
    snd_pcm_plugin_flush(pcm, SND_PCM_CHANNEL_PLAYBACK);
    snd_pcm_close(pcm);
    if (audioman) audio_manager_free_handle(audioman);
}

void recover(snd_pcm_t *pcm, QAtomicInt *underruns)
{
    snd_pcm_channel_status_t status;
    memset(&status, 0, sizeof status);
    status.channel = SND_PCM_CHANNEL_PLAYBACK;
    if (snd_pcm_plugin_status(pcm, &status) == 0
            && (status.status == SND_PCM_STATUS_READY || status.status == SND_PCM_STATUS_UNDERRUN)) {
        if (status.status == SND_PCM_STATUS_UNDERRUN) underruns->ref();
        snd_pcm_plugin_prepare(pcm, SND_PCM_CHANNEL_PLAYBACK);
    }
}

} // namespace

AudioOutput::AudioOutput(AudioRing *source, QObject *parent) :
        QThread(parent), m_source(source), m_stop(0)
{
}

void AudioOutput::run()
{
    const int startBytes = kStartMs * kBytesPerMs;
    const int maxBytes = kMaxMs * kBytesPerMs;
    while (!m_stop) {
        // Idle: the device stays closed until a start buffer has arrived.
        if (!m_source->waitFor(startBytes, 200)) continue;

        unsigned int audioman = 0;
        int frag = 0;
        m_error.clear();
        snd_pcm_t *pcm = openDevice(&audioman, &frag, &m_error);
        if (!pcm) {
            qWarning() << "[Feather:audio]" << m_error;
            msleep(1000);
            continue;
        }
        deviceOpen = 1;
        QByteArray buffer(frag, 0);
        bool prebuffering = false;
        QElapsedTimer starved; // since the ring ran dry
        starved.invalidate();

        while (!m_stop) {
            if (prebuffering) {
                // Ran dry (pause, end of the queue, a slow network): wait for
                // a start buffer again, without writing; the device stops by
                // itself. Long enough idle: give the audio back.
                if (!m_source->waitFor(startBytes, 50)) {
                    if (starved.isValid() && starved.elapsed() > kReleaseMs) break;
                    continue;
                }
                prebuffering = false;
                starved.invalidate();
            }
            // Bounded latency: a burst, or the sender's clock running fast.
            while (m_source->level() > maxBytes) {
                m_source->skip(frag);
                dropped.ref();
            }
            if (!m_source->waitFor(frag, 2 * kFragMs)) {
                if (m_source->level() == 0) {
                    prebuffering = true;
                    starved.start();
                    continue;
                }
            }
            const int got = m_source->read(buffer.data(), frag);
            if (got < frag) memset(buffer.data() + got, 0, frag - got);
            const ssize_t n = snd_pcm_plugin_write(pcm, buffer.constData(), frag);
            if (n < frag) recover(pcm, &underruns);
        }
        closeDevice(pcm, audioman);
        deviceOpen = 0;
    }
}
