#ifndef AUDIOOUTPUT_HPP_
#define AUDIOOUTPUT_HPP_

#include <QAtomicInt>
#include <QString>
#include <QThread>

#include "audioring.hpp"

// Plays the decoded stream (S16LE, 48 kHz stereo) from an AudioRing through
// libasound, as media (the audio manager's "multimedia" type: headphones or
// speaker, the media volume keys, paused by calls).
//
// The ring is the jitter buffer: playback starts once kStartMs are queued and
// keeps the level under kMaxMs. When the stream stops (pause, end of the
// queue) the device runs dry and, after kReleaseMs, is closed so other apps
// get the audio back; new data reopens it.
class AudioOutput : public QThread
{
    Q_OBJECT
public:
    // (an enum, not static const ints: qMin/qMax take references, which
    // would need out-of-class definitions)
    enum {
        kRate = 48000,
        kBytesPerMs = kRate * 4 / 1000,
        kStartMs = 250,
        kMaxMs = 1200,
        kReleaseMs = 10000
    };

    explicit AudioOutput(AudioRing *source, QObject *parent = 0);
    void requestStop() { m_stop = 1; m_source->wakeReaders(); }
    QString error() const { return m_error; }

    // Stats, for the log.
    QAtomicInt underruns;
    QAtomicInt dropped;
    QAtomicInt deviceOpen;

protected:
    void run();

private:
    AudioRing *m_source;
    QAtomicInt m_stop;
    QString m_error;
};

#endif /* AUDIOOUTPUT_HPP_ */
