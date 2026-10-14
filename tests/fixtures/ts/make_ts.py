#!/usr/bin/env python3
"""Makes the small MPEG-TS and Blu-ray .m2ts fixtures with ffmpeg (tests/test_local_probe.cpp).

    python3 make_ts.py        (ffmpeg with libx264, libx265, ac3, mp2, dca, truehd)

  av.ts       H.264 + AC-3 (eng) + MP2 (fra), 1 s
  av.m2ts     192-byte packets (the 4-byte header of a Blu-ray source packet), Blu-ray PIDs, AC-3 eng + AC-3 fra
  aac.ts      H.264 + AAC (ADTS, stream type 0x0F), 1 s
  hevc.ts     HEVC + AC-3          (cannot be played: no decoder)
  high10.ts   10-bit H.264 + AC-3  (cannot be played)
  hd.ts       H.264 + TrueHD + DTS, 1 s
"""
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    subprocess.run(cmd, shell=True, check=True, capture_output=True, cwd=HERE)


V = "-f lavfi -t 1 -i testsrc=size=64x64:rate=24"
A = "-f lavfi -t 1 -i sine=frequency=440:sample_rate=48000"
H264 = "-c:v libx264 -pix_fmt yuv420p -profile:v high -level 4.0 -g 12"


def main():
    base = ("ffmpeg -nostdin -v error -y %s %s %s -map 0:v -map 1:a -map 1:a %s "
            "-c:a:0 ac3 -ac:a:0 6 -b:a:0 192k -c:a:1 mp2 -ac:a:1 2 "
            "-metadata:s:a:0 language=eng -metadata:s:a:1 language=fra " % (V, A, "", H264))
    run(base + "-f mpegts av.ts")
    # Blu-ray PIDs (video 0x1011, audio 0x1100...); ffmpeg writes MPEG audio there as bare private data,
    # which a player cannot name, so the second track is Dolby Digital too
    m2 = base.replace("-c:a:1 mp2", "-c:a:1 ac3")
    run(m2 + "-f mpegts -mpegts_m2ts_mode 1 av.m2ts")
    run("ffmpeg -nostdin -v error -y %s %s -map 0:v -map 1:a %s -c:a aac -b:a 128k -metadata:s:a:0 language=eng -f mpegts aac.ts" % (V, A, H264))
    run("ffmpeg -nostdin -v error -y %s %s -map 0:v -map 1:a -c:v libx265 -pix_fmt yuv420p -c:a ac3 -f mpegts hevc.ts" % (V, A))
    run("ffmpeg -nostdin -v error -y %s %s -map 0:v -map 1:a -c:v libx264 -pix_fmt yuv420p10le -profile:v high10 -c:a ac3 -f mpegts high10.ts" % (V, A))
    run("ffmpeg -nostdin -v error -y %s %s -map 0:v -map 1:a -map 1:a %s -c:a:0 truehd -strict -2 -ac:a:0 6 "
        "-c:a:1 dca -strict -2 -ac:a:1 6 -b:a:1 768k -metadata:s:a:0 language=eng -metadata:s:a:1 language=eng -f mpegts hd.ts" % (V, A, H264))
    for f in sorted(os.listdir(HERE)):
        if f.endswith((".ts", ".m2ts")):
            print(f, os.path.getsize(os.path.join(HERE, f)))


if __name__ == "__main__":
    main()
