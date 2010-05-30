#include <cstdio>
#include <cstdlib>
#include <sys/time.h>
#include <time.h>
#include "config.h"

using namespace std;

#include <QFile>
#include "mythcorecontext.h"
#include "audiooutputalsa.h"

#define LOC QString("ALSA: ")
#define LOC_WARN QString("ALSA, Warning: ")
#define LOC_ERR QString("ALSA, Error: ")

// redefine assert as no-op to quiet some compiler warnings
// about assert always evaluating true in alsa headers.
#undef assert
#define assert(x)

#define IECSTATUS_AUDIO     true
#define IECSTATUS_NONAUDIO  false

#define CHANNELS_MIN 1
#define CHANNELS_MAX 8

#define OPEN_FLAGS SND_PCM_NO_AUTO_RESAMPLE|SND_PCM_NO_AUTO_FORMAT|\
                   SND_PCM_NO_AUTO_CHANNELS

#define AERROR(str)   VBERROR(str + QString(": %1").arg(snd_strerror(err)))
#define CHECKERR(str) { if (err < 0) { AERROR(str); return err; } }

AudioOutputALSA::AudioOutputALSA(const AudioSettings &settings) :
    AudioOutputBase(settings),
    pcm_handle(NULL),
    numbadioctls(0),
    pbufsize(-1),
    m_card(-1),
    m_device(-1),
    m_subdevice(-1)
{
    m_mixer.handle = NULL;
    m_mixer.elem = NULL;

    // Set everything up
    Reconfigure(settings);
}

AudioOutputALSA::~AudioOutputALSA()
{
    if (pbufsize > 0)
        SetPreallocBufferSize(pbufsize);
    KillAudio();
    SetIECStatus(IECSTATUS_AUDIO);
}

int AudioOutputALSA::GetPCMInfo(int &card, int &device, int &subdevice)
{
    // Check for saved values
    if (m_card != -1 && m_device != -1 && m_subdevice != -1)
    {
        card = m_card;
        device = m_device;
        subdevice = m_subdevice;
        return 0;
    }

    if (!pcm_handle)
        return -1;

    int err;
    snd_pcm_info_t *pcm_info = NULL;
    int tcard, tdevice, tsubdevice;

    snd_pcm_info_alloca(&pcm_info);

    err = snd_pcm_info(pcm_handle, pcm_info);
    CHECKERR("snd_pcm_info");

    err = snd_pcm_info_get_card(pcm_info);
    CHECKERR("snd_pcm_info_get_card");
    tcard = err;

    err = snd_pcm_info_get_device(pcm_info);
    CHECKERR("snd_pcm_info_get_device");
    tdevice = err;

    err = snd_pcm_info_get_subdevice(pcm_info);
    CHECKERR("snd_pcm_info_get_subdevice");
    tsubdevice = err;

    m_card = card = tcard;
    m_device = device = tdevice;
    m_subdevice = subdevice = tsubdevice;

    return 0;
 }

int AudioOutputALSA::SetIECStatus(bool audio)
{
    snd_ctl_t *ctl;
    const char *spdif_str = SND_CTL_NAME_IEC958("", PLAYBACK, DEFAULT);
    int spdif_index = -1;
    snd_ctl_elem_list_t *clist;
    snd_ctl_elem_id_t *cid;
    snd_ctl_elem_value_t *cval;
    snd_aes_iec958_t iec958;
    int cidx, controls, err, card, device, subdevice;

    VBAUDIO(QString("Setting IEC958 status: %1")
            .arg(audio ? "audio" : "non-audio"));

    QString pdevice = passthru || enc ? passthru_device : main_device;

    // Open PCM for GetPCMInfo
    if (pcm_handle)
    {
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
    }
    QByteArray dev_ba = pdevice.toAscii();
    err = snd_pcm_open(&pcm_handle, dev_ba.constData(),
                       SND_PCM_STREAM_PLAYBACK, OPEN_FLAGS);
    CHECKERR(QString("snd_pcm_open(\"%1\")").arg(pdevice));

    if ((err = GetPCMInfo(card, device, subdevice)) < 0)
    {
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
        return err;
    }

    snd_pcm_close(pcm_handle);
    pcm_handle = NULL;

    // Now open the ctl
    QByteArray ctln = QString("hw:CARD=%1").arg(card).toAscii();

    err = snd_ctl_open(&ctl, ctln.constData(), 0);
    CHECKERR(QString("Can't open CTL %1").arg(ctln.constData()));

    // Find the iec958 playback control
    snd_ctl_elem_list_alloca(&clist);
    snd_ctl_elem_list(ctl, clist);
    snd_ctl_elem_list_alloc_space(clist, snd_ctl_elem_list_get_count(clist));
    snd_ctl_elem_list(ctl, clist);
    controls = snd_ctl_elem_list_get_used(clist);

    for (cidx = 0; cidx < controls; cidx++)
    {
        if (!strcmp(snd_ctl_elem_list_get_name(clist, cidx), spdif_str))
            if (spdif_index < 0 ||
                snd_ctl_elem_list_get_index(clist, cidx) == (uint)spdif_index)
                    break;
    }

    if (cidx >= controls)
        return 0;

    snd_ctl_elem_id_alloca(&cid);
    snd_ctl_elem_list_get_id(clist, cidx, cid);
    snd_ctl_elem_value_alloca(&cval);
    snd_ctl_elem_value_set_id(cval, cid);
    snd_ctl_elem_read(ctl,cval);
    snd_ctl_elem_value_get_iec958(cval, &iec958);

    // Set audio / non copyright-mode
    if (!audio)
    {
        iec958.status[0] |= IEC958_AES0_NONAUDIO;
        iec958.status[0] |= IEC958_AES0_CON_NOT_COPYRIGHT;
    }
    else
        iec958.status[0] &= ~IEC958_AES0_NONAUDIO;

    snd_ctl_elem_value_set_iec958(cval, &iec958);
    snd_ctl_elem_write(ctl, cval);
    snd_ctl_close(ctl);
    return 0;
}

bool AudioOutputALSA::SetPreallocBufferSize(int size)
{
    int card, device, subdevice;
    bool ret = true;

    VBAUDIO(QString("Setting preallocated buffer size to %1").arg(size));

    if (GetPCMInfo(card, device, subdevice) < 0)
        return false;

    QFile pfile(QString("/proc/asound/card%1/pcm%2p/sub%3/prealloc")
                .arg(card).arg(device).arg(subdevice));

    if (!pfile.open(QIODevice::ReadWrite))
    {
        VBERROR(QString("Error opening %1: %2")
                .arg(pfile.fileName()).arg(pfile.errorString()));
        return false;
    }

    QByteArray content_ba = QString("%1\n").arg(size).toAscii();
    if (pfile.write(content_ba.constData()) <= 0)
    {
        VBERROR(QString("Error writing to %1")
                .arg(pfile.fileName()).arg(pfile.errorString()));
        ret = false;
    }

    pfile.close();

    return ret;
}

bool AudioOutputALSA::IncPreallocBufferSize(int buffer_time)
{
    int card, device, subdevice;
    bool ret = true;
    pbufsize = 0;

    if (GetPCMInfo(card, device, subdevice) < 0)
        return false;

    const QString pf = QString("/proc/asound/card%1/pcm%2p/sub%3/prealloc")
                       .arg(card).arg(device).arg(subdevice);

    QFile pfile(pf);
    QFile mfile(pf + "_max");

    if (!pfile.open(QIODevice::ReadOnly))
    {
        VBERROR(QString("Error opening %1").arg(pf));
        return false;
    }

    if (!mfile.open(QIODevice::ReadOnly))
    {
        VBERROR(QString("Error opening %1").arg(pf + "_max"));
        return false;
    }

    int cur  = pfile.readAll().trimmed().toInt();
    int max  = mfile.readAll().trimmed().toInt();
    int size = cur * (50000 / buffer_time + 1);

    VBAUDIO(QString("Prealloc buffer cur: %1 max: %3").arg(cur).arg(max));

    if (size > max)
    {
        size = max;
        ret = false;
    }

    if (!size)
        ret = false;

    pfile.close();
    mfile.close();

    if (SetPreallocBufferSize(size))
        pbufsize = cur;
    else
        ret = false;

    return ret;
}

AudioOutputSettings* AudioOutputALSA::GetOutputSettings()
{
    snd_pcm_hw_params_t *params;
    snd_pcm_format_t afmt = SND_PCM_FORMAT_UNKNOWN;
    AudioFormat fmt;
    int rate;
    int err;

    QString real_device = (passthru || enc) ? passthru_device : main_device;

    AudioOutputSettings *settings = new AudioOutputSettings();

    VBAUDIO(QString("GetOutputSettings() opening %1").arg(real_device));

    if (pcm_handle)
    {
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
    }
    QByteArray dev_ba = real_device.toAscii();
    if((err = snd_pcm_open(&pcm_handle, dev_ba.constData(),
                           SND_PCM_STREAM_PLAYBACK, OPEN_FLAGS)) < 0)
    {
        AERROR(QString("snd_pcm_open(\"%1\")").arg(real_device));
        return settings;
    }

    snd_pcm_hw_params_alloca(&params);

    if ((err = snd_pcm_hw_params_any(pcm_handle, params)) < 0)
    {
        AERROR("No playback configurations available");
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
        return settings;
    }

    while ((rate = settings->GetNextRate()))
        if(snd_pcm_hw_params_test_rate(pcm_handle, params, rate, 0) >= 0)
            settings->AddSupportedRate(rate);

    while ((fmt = settings->GetNextFormat()))
    {
        switch (fmt)
        {
            case FORMAT_U8:  afmt = SND_PCM_FORMAT_U8;    break;
            case FORMAT_S16: afmt = SND_PCM_FORMAT_S16;   break;
            case FORMAT_S24: afmt = SND_PCM_FORMAT_S24;   break;
            case FORMAT_S32: afmt = SND_PCM_FORMAT_S32;   break;
            case FORMAT_FLT: afmt = SND_PCM_FORMAT_FLOAT; break;
            default:         continue;
        }
        if (snd_pcm_hw_params_test_format(pcm_handle, params, afmt) >= 0)
            settings->AddSupportedFormat(fmt);
    }

    for (uint channels = CHANNELS_MIN; channels <= CHANNELS_MAX; channels++)
        if (snd_pcm_hw_params_test_channels(pcm_handle, params, channels) >= 0)
            settings->AddSupportedChannels(channels);

    snd_pcm_close(pcm_handle);
    pcm_handle = NULL;

    return settings;
}

bool AudioOutputALSA::OpenDevice()
{
    snd_pcm_format_t format;
    uint buffer_time, period_time;
    int err;
    QString real_device;

    if (pcm_handle != NULL)
        CloseDevice();

    numbadioctls = 0;

    if (passthru || enc)
    {
        real_device = passthru_device;
        SetIECStatus(IECSTATUS_NONAUDIO);
    }
    else
    {
        real_device = main_device;
        SetIECStatus(IECSTATUS_AUDIO);
    }

    VERBOSE(VB_GENERAL, QString("Opening ALSA audio device '%1'.")
                        .arg(real_device));

    QByteArray dev_ba = real_device.toAscii();
    if ((err = snd_pcm_open(&pcm_handle, dev_ba.constData(),
                            SND_PCM_STREAM_PLAYBACK, OPEN_FLAGS)) < 0)
    {
        AERROR(QString("snd_pcm_open(%1)").arg(real_device));
        if (pcm_handle)
            CloseDevice();
        return false;
    }

    switch (output_format)
    {
        case FORMAT_U8:  format = SND_PCM_FORMAT_U8;    break;
        case FORMAT_S16: format = SND_PCM_FORMAT_S16;   break;
        case FORMAT_S24: format = SND_PCM_FORMAT_S24;   break;
        case FORMAT_S32: format = SND_PCM_FORMAT_S32;   break;
        case FORMAT_FLT: format = SND_PCM_FORMAT_FLOAT; break;
        default:
            Error(QString("Unknown sample format: %1").arg(output_format));
            return false;
    }

    period_time = 50000;  // aim for an interrupt every 50ms
    buffer_time = period_time << 1; // buffer 100ms worth of samples

    err = SetParameters(pcm_handle, format, channels, samplerate,
                        buffer_time, period_time);
    if (err < 0)
    {
        AERROR("Unable to set ALSA parameters");
        CloseDevice();
        return false;
    }
    else if (err > 0 && pbufsize < 0)
    {
        // We need to increase preallocated buffer size in the driver
        // Set it and try again
        if(!IncPreallocBufferSize(err))
            VBERROR("Unable to sufficiently increase preallocated buffer size"
                    " - underruns are likely");
        return OpenDevice();
    }

    if (internal_vol && !OpenMixer())
    {
        CloseDevice();
        return false;
    }

    // Device opened successfully
    return true;
}

void AudioOutputALSA::CloseDevice()
{
    if (m_mixer.handle)
        snd_mixer_close(m_mixer.handle);
    m_mixer.handle = NULL;
    if (pcm_handle)
    {
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
    }
}

template <class AudioDataType>
static inline void _ReorderSmpteToAlsa(AudioDataType *buf, uint frames,
                                         uint extrach)
{
    AudioDataType tmpC, tmpLFE, *buf2;

    for (uint i = 0; i < frames; i++)
    {
        buf = buf2 = buf + 2;

        tmpC = *buf++;
        tmpLFE = *buf++;
        *buf2++ = *buf++;
        *buf2++ = *buf++;
        *buf2++ = tmpC;
        *buf2++ = tmpLFE;
        buf += extrach;
    }
}

static inline void ReorderSmpteToAlsa(void *buf, uint frames,
                                        AudioFormat format, uint extrach)
{
    switch(AudioOutputSettings::FormatToBits(format))
    {
        case  8: _ReorderSmpteToAlsa((uchar *)buf, frames, extrach); break;
        case 16: _ReorderSmpteToAlsa((short *)buf, frames, extrach); break;
        default: _ReorderSmpteToAlsa((int   *)buf, frames, extrach); break;
    }
}

void AudioOutputALSA::WriteAudio(uchar *aubuf, int size)
{
    uchar *tmpbuf = aubuf;
    uint frames = size / output_bytes_per_frame;
    int err, lw = 0;

    if (pcm_handle == NULL)
    {
        Error("WriteAudio() called with pcm_handle == NULL!");
        return;
    }

    if ((!passthru && channels == 6) || channels == 8)
        ReorderSmpteToAlsa(aubuf, frames, output_format, channels - 6);

    VERBOSE(VB_AUDIO+VB_TIMESTAMP,
            QString("WriteAudio: Preparing %1 bytes (%2 frames)")
            .arg(size).arg(frames));

    while (frames > 0)
    {
        lw = pcm_write_func(pcm_handle, tmpbuf, frames);

        if (lw >= 0)
        {
            if ((uint)lw < frames)
                VBAUDIO(QString("WriteAudio: short write %1 bytes (ok)")
                        .arg(lw * output_bytes_per_frame));

            frames -= lw;
            tmpbuf += lw * output_bytes_per_frame; // bytes
            continue;
        }

        err = lw;

        switch (err)
        {
            case -EPIPE:
                 if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_XRUN)
                 {
                    VBERROR("WriteAudio: buffer underrun");
                    if ((err = snd_pcm_prepare(pcm_handle)) < 0)
                    {
                        AERROR("WriteAudio: unable to recover from xrun");
                        return;
                    }
                }
                break;

            case -ESTRPIPE:
                VBAUDIO("WriteAudio: device is suspended");
                while ((err = snd_pcm_resume(pcm_handle)) == -EAGAIN)
                    usleep(200);

                if (err < 0)
                {
                    VBERROR("WriteAudio: resume failed");
                    if ((err = snd_pcm_prepare(pcm_handle)) < 0)
                    {
                        AERROR("WriteAudio: unable to recover from suspend");
                        return;
                    }
                }
                break;

            case -EBADFD:
                Error(
                    QString("WriteAudio: device is in a bad state (state = %1)")
                    .arg(snd_pcm_state(pcm_handle)));
                return;

            default:
                AERROR(QString("WriteAudio: Write failed, state: %1, err")
                       .arg(snd_pcm_state(pcm_handle)));
                return;
        }
    }
}

int AudioOutputALSA::GetBufferedOnSoundcard(void) const
{
    if (pcm_handle == NULL)
    {
        VBERROR("getBufferedOnSoundcard() called with pcm_handle == NULL!");
        return 0;
    }

    snd_pcm_sframes_t delay = 0;

    snd_pcm_state_t state = snd_pcm_state(pcm_handle);
    if (state == SND_PCM_STATE_RUNNING || state == SND_PCM_STATE_DRAINING)
        snd_pcm_delay(pcm_handle, &delay);

    if (delay <= 0)
        return 0;

    return delay * output_bytes_per_frame;
}

int AudioOutputALSA::SetParameters(snd_pcm_t *handle, snd_pcm_format_t format,
                                   uint channels, uint rate, uint buffer_time,
                                   uint period_time)
{
    int err, dir;
    snd_pcm_hw_params_t *params;
    snd_pcm_sw_params_t *swparams;
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;

    VBAUDIO(QString("SetParameters(format=%1, channels=%2, rate=%3, "
                    "buffer_time=%4, period_time=%5)")
            .arg(format).arg(channels).arg(rate).arg(buffer_time)
            .arg(period_time));

    if (handle == NULL)
    {
        Error("SetParameters() called with handle == NULL!");
        return 0;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_sw_params_alloca(&swparams);

    /* choose all parameters */
    err = snd_pcm_hw_params_any(handle, params);
    CHECKERR("No playback configurations available");

    /* set the interleaved read/write format, use mmap if available */
    pcm_write_func = &snd_pcm_mmap_writei;
    if ((err = snd_pcm_hw_params_set_access(handle, params,
                                          SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0)
    {
        Warn("mmap not available, attempting to fall back to slow writes");
        QString old_err = snd_strerror(err);
        pcm_write_func  = &snd_pcm_writei;
        if ((err = snd_pcm_hw_params_set_access(handle, params,
                                            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        {
            Error("Interleaved sound types MMAP & RW are not available");
            AERROR(QString("MMAP Error: %1\n\t\t\tRW Error").arg(old_err));
            return err;
        }
    }

    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    CHECKERR(QString("Sample format %1 not available").arg(format));

    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    CHECKERR(QString("Channels count %1 not available").arg(channels));

    /* set the stream rate */
    err = snd_pcm_hw_params_set_rate(handle, params, rate, 0);
    CHECKERR(QString("Samplerate %1 Hz not available").arg(rate));

    /* set the buffer time */
    err = snd_pcm_hw_params_set_buffer_time_near(handle, params,
                                                 &buffer_time, &dir);
    CHECKERR(QString("Unable to set buffer time %1").arg(buffer_time));

    // See if we need to increase the prealloc'd buffer size
    // If buffer_time is too small we could underrun
    if (buffer_time < 50000 && pbufsize < 0)
        return buffer_time;

    VBAUDIO(QString("Buffer time = %1 us").arg(buffer_time));

    /* set the period time */
    err = snd_pcm_hw_params_set_period_time_near(handle, params,
                                                 &period_time, &dir);
    CHECKERR(QString("Unable to set period time %1").arg(period_time));
    VBAUDIO(QString("Period time = %1 us").arg(period_time));

    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    CHECKERR("Unable to set hw params for playback");

    err = snd_pcm_get_params(handle, &buffer_size, &period_size);
    CHECKERR("Unable to get PCM params");
    VBAUDIO(QString("Buffer size = %1 | Period size = %2")
            .arg(buffer_size).arg(period_size));

    /* set member variables */
    soundcard_buffer_size = buffer_size * output_bytes_per_frame;
    fragment_size = (period_size * output_bytes_per_frame) >> 1;

    /* get the current swparams */
    err = snd_pcm_sw_params_current(handle, swparams);
    CHECKERR("Unable to get current swparams");

    /* start the transfer after period_size */
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, buffer_size);
    CHECKERR("Unable to set start threshold");

    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
    CHECKERR("Unable to set avail min");

    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    CHECKERR("Unable to set sw params");

    err = snd_pcm_prepare(handle);
    CHECKERR("Unable to prepare the PCM");

    return 0;
}

int AudioOutputALSA::GetVolumeChannel(int channel) const
{
    int retvol = 0;

    if (!m_mixer.elem)
        return retvol;

    snd_mixer_selem_channel_id_t chan = (snd_mixer_selem_channel_id_t) channel;
    if (!snd_mixer_selem_has_playback_channel(m_mixer.elem, chan))
        return retvol;

    long mixervol;
    int chk;
    if ((chk = snd_mixer_selem_get_playback_volume(m_mixer.elem,
                                                   chan,
                                                   &mixervol)) < 0)
    {
        VBERROR(QString("failed to get channel %1 volume, mixer %2/%3: %4")
                .arg(channel).arg(m_mixer.device)
                .arg(m_mixer.control)
                .arg(snd_strerror(chk)));
    }
    else
    {
        retvol = (m_mixer.volrange != 0L) ? (mixervol - m_mixer.volmin) *
                                            100.0f / m_mixer.volrange + 0.5f
                                            : 0;
        retvol = max(retvol, 0);
        retvol = min(retvol, 100);
        VBAUDIO(QString("get volume channel %1: %2")
                .arg(channel).arg(retvol));
    }
    return retvol;
}

void AudioOutputALSA::SetVolumeChannel(int channel, int volume)
{
    if (!(internal_vol && m_mixer.elem))
        return;

    long mixervol = volume * m_mixer.volrange / 100.0f - m_mixer.volmin + 0.5f;
    mixervol = max(mixervol, m_mixer.volmin);
    mixervol = min(mixervol, m_mixer.volmax);

    snd_mixer_selem_channel_id_t chan = (snd_mixer_selem_channel_id_t) channel;
    if (snd_mixer_selem_set_playback_volume(m_mixer.elem, chan, mixervol) < 0)
        VBERROR(QString("failed to set channel %1 volume").arg(channel));
    else
        VBAUDIO(QString("channel %1 volume set %2 => %3")
                .arg(channel).arg(volume).arg(mixervol));
}

bool AudioOutputALSA::OpenMixer(void)
{
    if (!pcm_handle)
    {
        VBERROR("mixer setup without a pcm");
        return false;
    }
    m_mixer.device = gCoreContext->GetSetting("MixerDevice", "default");
    m_mixer.device = m_mixer.device.remove(QString("ALSA:"));
    if (m_mixer.device.toLower() == "software")
        return true;

    m_mixer.control = gCoreContext->GetSetting("MixerControl", "PCM");

    QString mixer_device_tag = QString("mixer device %1").arg(m_mixer.device);

    int chk;
    if ((chk = snd_mixer_open(&m_mixer.handle, 0)) < 0)
    {
        VBERROR(QString("failed to open mixer device %1: %2")
                .arg(mixer_device_tag).arg(snd_strerror(chk)));
        return false;
    }

    QByteArray dev_ba = m_mixer.device.toAscii();
    struct snd_mixer_selem_regopt regopts =
        {1, SND_MIXER_SABSTRACT_NONE, dev_ba.constData(), NULL, NULL};

    if ((chk = snd_mixer_selem_register(m_mixer.handle, &regopts, NULL)) < 0)
    {
        snd_mixer_close(m_mixer.handle);
        m_mixer.handle = NULL;
        VBERROR(QString("failed to register %1: %2")
                .arg(mixer_device_tag).arg(snd_strerror(chk)));
        return false;
    }

    if ((chk = snd_mixer_load(m_mixer.handle)) < 0)
    {
        snd_mixer_close(m_mixer.handle);
        m_mixer.handle = NULL;
        VBERROR(QString("failed to load %1: %2")
                .arg(mixer_device_tag).arg(snd_strerror(chk)));
        return false;
    }

    m_mixer.elem = NULL;
    uint elcount = snd_mixer_get_count(m_mixer.handle);
    snd_mixer_elem_t* elx = snd_mixer_first_elem(m_mixer.handle);

    for (uint ctr = 0; elx != NULL && ctr < elcount; ctr++)
    {
        QString tmp = QString(snd_mixer_selem_get_name(elx));
        if (m_mixer.control == tmp &&
            !snd_mixer_selem_is_enumerated(elx) &&
            snd_mixer_selem_has_playback_volume(elx) &&
            snd_mixer_selem_is_active(elx))
        {
            m_mixer.elem = elx;
            VBAUDIO(QString("found playback control %1 on %2")
                    .arg(m_mixer.control)
                    .arg(mixer_device_tag));
            break;
        }
        elx = snd_mixer_elem_next(elx);
    }
    if (!m_mixer.elem)
    {
        snd_mixer_close(m_mixer.handle);
        m_mixer.handle = NULL;
        VBERROR(QString("no playback control %1 found on %2")
                .arg(m_mixer.control).arg(mixer_device_tag));
        return false;
    }
    if ((snd_mixer_selem_get_playback_volume_range(m_mixer.elem,
                                                   &m_mixer.volmin,
                                                   &m_mixer.volmax) < 0))
    {
        snd_mixer_close(m_mixer.handle);
        m_mixer.handle = NULL;
        VBERROR(QString("failed to get volume range on %1/%2")
                .arg(mixer_device_tag).arg(m_mixer.control));
        return false;
    }

    m_mixer.volrange = m_mixer.volmax - m_mixer.volmin;
    VBAUDIO(QString("mixer volume range on %1/%2 - min %3, max %4, range %5")
            .arg(mixer_device_tag).arg(m_mixer.control)
            .arg(m_mixer.volmin).arg(m_mixer.volmax).arg(m_mixer.volrange));
    VBAUDIO(QString("%1/%2 set up successfully")
            .arg(mixer_device_tag)
            .arg(m_mixer.control));

    if (set_initial_vol)
    {
        int initial_vol;
        if (m_mixer.control == "PCM")
            initial_vol = gCoreContext->GetNumSetting("PCMMixerVolume", 80);
        else
            initial_vol = gCoreContext->GetNumSetting("MasterMixerVolume", 80);
        for (int ch = 0; ch < channels; ++ch)
            SetVolumeChannel(ch, initial_vol);
    }

    return true;
}

QMap<QString, QString> *AudioOutputALSA::GetALSADevices(const char *type)
{
    QMap<QString, QString> *alsadevs = new QMap<QString, QString>();
    void **hints, **n;
    char *name, *desc;

    if (snd_device_name_hint(-1, type, &hints) < 0)
        return alsadevs;
    n = hints;

    while (*n != NULL)
    {
          name = snd_device_name_get_hint(*n, "NAME");
          desc = snd_device_name_get_hint(*n, "DESC");
          VBAUDIO(QString("Found ALSA device: %1, (%2)").arg(name).arg(desc));
          alsadevs->insert(name, desc);
          if (name != NULL)
              free(name);
          if (desc != NULL)
              free(desc);
          n++;
    }
    snd_device_name_free_hint(hints);
        // Work around ALSA bug < 1.0.22 ; where snd_device_name_hint can corrupt
        // global ALSA memory context
#if SND_LIB_MAJOR == 1
#if SND_LIB_MINOR == 0
#if SND_LIB_SUBMINOR < 22
    snd_config_update_free_global();
#endif
#endif
#endif
    return alsadevs;
}
