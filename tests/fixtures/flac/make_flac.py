#!/usr/bin/env python3
"""Makes the FLAC fixtures with ffmpeg's encoder (tests/test_flac.cpp).

    python3 make_flac.py        (ffmpeg with the flac encoder)

All short.  The test decodes every one and compares each sample with ffmpeg's decoder and the
MD5 the encoder wrote into STREAMINFO.

  tone16.flac       stereo 44.1 kHz 16-bit, two tones and a little noise (LPC, mid/side)
  noise16.flac      stereo 44.1 kHz full-scale white noise (verbatim and escape-coded partitions)
  silence.flac      stereo 44.1 kHz zeros (constant subframes)
  tone24.flac       stereo 96 kHz 24-bit
  surround16.flac   5.1 48 kHz, a different tone in every channel
  mono22.flac       mono 22.05 kHz
  wasted12.flac     16-bit samples with the low 4 bits zero (wasted bits)
  ls.flac rs.flac ms.flat.flac indep.flac   the four stereo modes forced
  fixed.flac        compression level 0 (fixed predictors)
  fs100.flac fs1152.flac fs5000.flac        block sizes coded 8-bit, from the table, 16-bit
"""
import math
import os
import random
import struct
import subprocess
import wave

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    subprocess.run(cmd, shell=True, check=True, capture_output=True, cwd=HERE)


def write_wav(name, rate, channels, width, frames):
    w = wave.open(os.path.join(HERE, name), "wb")
    w.setnchannels(channels)
    w.setsampwidth(width)
    w.setframerate(rate)
    data = bytearray()
    for fr in frames:
        for v in fr:
            if width == 2:
                data += struct.pack("<h", v)
            else:
                data += struct.pack("<i", v)[:3] if False else struct.pack("<i", v << 8)[1:]
    w.writeframes(bytes(data))
    w.close()


def main():
    rnd = random.Random(1234)
    n = 11025
    tone = []
    for i in range(n):
        t = i / 44100.0
        l = 0.5 * math.sin(2 * math.pi * 440 * t) + 0.1 * math.sin(2 * math.pi * 3000 * t)
        r = 0.4 * math.sin(2 * math.pi * 660 * t) + 0.01 * (rnd.random() - 0.5)
        tone.append((int(l * 32767), int(r * 32767)))
    write_wav("tone16.wav", 44100, 2, 2, tone)
    write_wav("noise16.wav", 44100, 2, 2, [(rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(3000)])
    write_wav("silence.wav", 44100, 2, 2, [(0, 0)] * 3000)
    t24 = []
    for i in range(9600):
        t = i / 96000.0
        t24.append((int(0.4 * math.sin(2 * math.pi * 1000 * t) * 8388607), int((0.3 * math.sin(2 * math.pi * 7000 * t) + 0.1 * math.sin(2 * math.pi * 21000 * t)) * 8388607)))
    write_wav("tone24.wav", 96000, 2, 3, t24)
    write_wav("wasted12.wav", 44100, 2, 2, [((l >> 4) << 4, (r >> 4) << 4) for l, r in tone[:6000]])

    run("ffmpeg -nostdin -v error -y -i tone16.wav -c:a flac -compression_level 8 tone16.flac")
    run("ffmpeg -nostdin -v error -y -i noise16.wav -c:a flac -compression_level 5 noise16.flac")
    run("ffmpeg -nostdin -v error -y -i silence.wav -c:a flac silence.flac")
    run("ffmpeg -nostdin -v error -y -i tone24.wav -c:a flac -compression_level 8 tone24.flac")
    run("ffmpeg -nostdin -v error -y -i wasted12.wav -c:a flac wasted12.flac")
    tones = [300, 500, 700, 80, 1100, 1300]
    inputs = " ".join("-f lavfi -t 0.25 -i sine=frequency=%d:sample_rate=48000" % f for f in tones)
    filt = "".join("[%d]" % i for i in range(6)) + "amerge=inputs=6,channelmap=map=0|1|2|3|4|5:channel_layout=5.1[a]"
    run('ffmpeg -nostdin -v error -y %s -filter_complex "%s" -map "[a]" -sample_fmt s16 -c:a flac surround16.flac' % (inputs, filt))
    run("ffmpeg -nostdin -v error -y -f lavfi -t 0.25 -i sine=frequency=800:sample_rate=22050 -ac 1 -sample_fmt s16 -c:a flac mono22.flac")
    for mode, name in (("left_side", "ls"), ("right_side", "rs"), ("mid_side", "ms"), ("indep", "indep")):
        run("ffmpeg -nostdin -v error -y -i tone16.wav -t 0.15 -c:a flac -ch_mode %s %s.flac" % (mode, name))
    run("ffmpeg -nostdin -v error -y -i tone16.wav -t 0.3 -c:a flac -compression_level 0 fixed.flac")
    for fs in (100, 1152, 5000):
        run("ffmpeg -nostdin -v error -y -i tone16.wav -t 0.3 -c:a flac -frame_size %d fs%d.flac" % (fs, fs))
    # a Matroska file with FLAC and every PCM layout the player carries (test_lossless.cpp, test_mkv_ts.cpp):
    #   a1 FLAC stereo 44.1 kHz 1000 Hz | a2 PCM s16le stereo 44.1 kHz 1000 Hz | a3 PCM s24le 5.1 16 kHz, a tone per channel |
    #   a4 PCM f32le mono 32 kHz 800 Hz | a5 PCM s16be stereo 22.05 kHz, 440 Hz left and 880 Hz right
    def tone(f, rate):
        return "-f lavfi -t 0.25 -i sine=frequency=%d:sample_rate=%d" % (f, rate)
    inputs = [tone(1000, 44100), tone(1000, 44100)] + [tone(f, 16000) for f in tones] + [tone(800, 32000), tone(440, 22050), tone(880, 22050)]
    run("ffmpeg -nostdin -v error -y -f lavfi -t 1 -i testsrc=size=64x64:rate=24 " + " ".join(inputs) + " "
        '-filter_complex "[3][4][5][6][7][8]amerge=inputs=6,channelmap=map=0|1|2|3|4|5:channel_layout=5.1[m];'
        '[10][11]amerge=inputs=2,channelmap=map=0|1:channel_layout=stereo[st]" '
        '-map 0:v -map 1:a -map 2:a -map "[m]" -map 9:a -map "[st]" '
        "-c:v libx264 -pix_fmt yuv420p -g 12 -ac:a:0 2 -ac:a:1 2 -c:a:0 flac -c:a:1 pcm_s16le -c:a:2 pcm_s24le -c:a:3 pcm_f32le -c:a:4 pcm_s16be flac_pcm.mkv")
    for f in os.listdir(HERE):
        if f.endswith(".wav"):
            os.remove(os.path.join(HERE, f))
    for f in sorted(os.listdir(HERE)):
        if f.endswith((".flac", ".mkv")):
            print(f, os.path.getsize(os.path.join(HERE, f)))


if __name__ == "__main__":
    main()
