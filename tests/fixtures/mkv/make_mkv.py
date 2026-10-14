#!/usr/bin/env python3
"""Makes the small Matroska fixtures with ffmpeg and records what ffprobe counts in them.

    python3 make_mkv.py        (ffmpeg with libx264/ac3/mp3lame/dca/flac/aac, ffprobe)

Files (a few KB each, committed):
  av.mkv       64x64 H.264 24 fps 2 s, AC-3 5.1, MP3 stereo (second audio), SRT subtitle; Cues at the end
  live.mkv     the same written to a pipe: no Cues, no Duration (an unseekable output)
  dts.mkv      H.264 + DTS (the encoder is experimental) + FLAC
  expect.txt   per fixture: "<file> <track> <codec_type> <codec> <packets> <first_pts_s>" from ffprobe
The expectations come from ffprobe, not from the code under test (tests/test_mkv_demux.cpp).
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd, **kw):
    return subprocess.run(cmd, shell=True, check=True, capture_output=True, cwd=HERE, **kw)


SRT = """1
00:00:00,200 --> 00:00:00,900
First line

2
00:00:01,000 --> 00:00:01,800
Second line
with two rows
"""

V = "-f lavfi -t 2 -i testsrc=size=64x64:rate=24"
A = "-f lavfi -t 2 -i sine=frequency=440:sample_rate=48000"


def main():
    open(os.path.join(HERE, "sub.srt"), "w").write(SRT)
    enc_v = "-c:v libx264 -pix_fmt yuv420p -profile:v high -level 4.0 -g 12 -bf 2 -x264opts keyint=12:min-keyint=12"
    run("ffmpeg -nostdin -v error -y %s %s %s -i sub.srt -map 0:v -map 1:a -map 1:a -map 2:s "
        "-c:v libx264 -pix_fmt yuv420p -profile:v high -level 4.0 -g 12 -bf 2 "
        "-c:a:0 ac3 -ac:a:0 6 -b:a:0 192k -c:a:1 libmp3lame -ac:a:1 2 -c:s srt "
        "-metadata:s:a:0 language=eng -metadata:s:a:1 language=fra -metadata:s:s:0 language=eng av.mkv" % (V, A, ""))
    run("ffmpeg -nostdin -v error -y %s %s -i sub.srt -map 0:v -map 1:a -map 2:s "
        "-c:v libx264 -pix_fmt yuv420p -g 12 -c:a ac3 -c:s srt -f matroska pipe:1 > live.mkv" % (V, A))
    run("ffmpeg -nostdin -v error -y %s %s %s -map 0:v -map 1:a -map 1:a "
        "%s -c:a:0 dca -strict -2 -ac:a:0 6 -b:a:0 768k -c:a:1 flac -t 1 -f matroska dts.mkv" % (V, A, "", enc_v))
    os.remove(os.path.join(HERE, "sub.srt"))

    lines = []
    for name in ("av.mkv", "live.mkv", "dts.mkv"):
        out = run("ffprobe -v error -count_packets -show_entries "
                  "stream=index,codec_type,codec_name,nb_read_packets,start_time -of json %s" % name).stdout
        for s in json.loads(out)["streams"]:
            lines.append("%s %d %s %s %s %s" % (name, int(s["index"]) + 1, s["codec_type"], s["codec_name"],
                                                s["nb_read_packets"], s.get("start_time", "N/A")))
    open(os.path.join(HERE, "expect.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    for name in ("av.mkv", "live.mkv", "dts.mkv"):
        print(name, os.path.getsize(os.path.join(HERE, name)))


if __name__ == "__main__":
    main()
