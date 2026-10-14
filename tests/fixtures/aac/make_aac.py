#!/usr/bin/env python3
"""Makes the AAC fixtures with ffmpeg's own AAC encoder (tests/test_aac.cpp, tests/test_mkv_ts.cpp).

    python3 make_aac.py        (ffmpeg with the aac encoder, libx264)

  stereo44.aac    ADTS, 44.1 kHz stereo, a 1000 Hz tone, 1 s
  stereo48.aac    ADTS, 48 kHz stereo, 440 Hz left and 880 Hz right, 1 s
  mono32.aac      ADTS, 32 kHz mono, 800 Hz, 1 s
  surround48.aac  ADTS, 48 kHz 5.1, a different tone in every channel (FL 300, FR 500, FC 700, LFE 80,
                  SL 1100, SR 1300 Hz), 1 s
  aac.mkv         H.264 64x64 24 fps 2 s with AAC 44.1 kHz stereo (1000 Hz) and AAC 48 kHz 5.1 as the second track
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    subprocess.run(cmd, shell=True, check=True, capture_output=True, cwd=HERE)


def sine(freq, rate, secs):
    return "-f lavfi -t %d -i sine=frequency=%d:sample_rate=%d" % (secs, freq, rate)


def main():
    run("ffmpeg -nostdin -v error -y %s -ac 2 -c:a aac -b:a 128k -f adts stereo44.aac" % sine(1000, 44100, 1))
    run('ffmpeg -nostdin -v error -y %s %s -filter_complex "[0][1]amerge=inputs=2,channelmap=map=0|1:channel_layout=stereo[a]" '
        '-map "[a]" -c:a aac -b:a 128k -f adts stereo48.aac' % (sine(440, 48000, 1), sine(880, 48000, 1)))
    run("ffmpeg -nostdin -v error -y %s -ac 1 -c:a aac -b:a 64k -f adts mono32.aac" % sine(800, 32000, 1))
    tones = [300, 500, 700, 80, 1100, 1300]
    surround_inputs = " ".join(sine(f, 48000, 1) for f in tones)
    surround_filter = "".join("[%d]" % i for i in range(6)) + "amerge=inputs=6,channelmap=map=0|1|2|3|4|5:channel_layout=5.1[a]"
    run('ffmpeg -nostdin -v error -y %s -filter_complex "%s" -map "[a]" -c:a aac -b:a 320k -f adts surround48.aac'
        % (surround_inputs, surround_filter))
    run('ffmpeg -nostdin -v error -y -f lavfi -t 2 -i testsrc=size=64x64:rate=24 %s %s '
        '-filter_complex "[1]aformat=channel_layouts=stereo[s];[%s]amerge=inputs=6,channelmap=map=0|1|2|3|4|5:channel_layout=5.1[m]" '
        '-map 0:v -map "[s]" -map "[m]" -c:v libx264 -pix_fmt yuv420p -profile:v high -level 4.0 -g 12 '
        '-c:a:0 aac -b:a:0 128k -c:a:1 aac -b:a:1 320k -metadata:s:a:0 language=eng -metadata:s:a:1 language=eng aac.mkv'
        % (sine(1000, 44100, 2),
           " ".join(sine(f, 48000, 2) for f in tones),
           "2:a][3:a][4:a][5:a][6:a][7:a"))
    for f in sorted(os.listdir(HERE)):
        if f.endswith((".aac", ".mkv")):
            print(f, os.path.getsize(os.path.join(HERE, f)))


if __name__ == "__main__":
    main()
