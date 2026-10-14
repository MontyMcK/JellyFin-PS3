#!/usr/bin/env python3
"""Write an XMLTV guide of made-up programmes for the Live TV test channels.

The test server's M3U tuner (test-channels.m3u) has four channels and no
guide; with this file added as an XMLTV listing provider the Live TV tab has
something to show for "now / next" and for the guide grid.

    python tools/gen_test_guide.py [output.xml] [days]

The programmes are deterministic (the same output on every run for the same
day) so a screenshot can be compared with the last one.  Times are UTC.
Nothing here talks to a server: adding the file as a provider is a change to
the server's configuration and is done by hand (Dashboard > Live TV > TV Guide
Data Providers > XMLTV), not by this script.
"""
import datetime
import random
import sys
from xml.sax.saxutils import escape

# tvg-id values in test-channels.m3u, with a display name each
CHANNELS = [
    ("nasa1", "NASA TV Public"),
    ("nasa2", "NASA TV Media"),
    ("redbull", "Red Bull TV"),
    ("bipbop", "Apple Test Pattern"),
]

TITLES = [
    "Morning Briefing", "Deep Space Report", "Launch Window", "Mission Control Live",
    "The Long Orbit", "Rover Diaries", "Skyline Hour", "Night Flight",
    "Engineering Hour", "Weather From Above", "Crew Conversations", "Test Card",
]
EPISODES = ["", "", "", "The Burn", "Re-entry", "Open Channel", "Ground Track", "Handover"]
DURATIONS_MIN = [30, 30, 60, 60, 90, 120]


def fmt(t):
    return t.strftime("%Y%m%d%H%M%S +0000")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test-guide.xml"
    days = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    today = datetime.datetime.now(datetime.timezone.utc).replace(
        hour=0, minute=0, second=0, microsecond=0)
    start = today - datetime.timedelta(days=1)         # a day of past, so "now" is mid-programme
    end = today + datetime.timedelta(days=days)

    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<tv generator-info-name="jellyfin-ps3 test guide">']
    for cid, name in CHANNELS:
        lines.append('  <channel id="%s"><display-name>%s</display-name></channel>'
                     % (escape(cid), escape(name)))
    for cid, _ in CHANNELS:
        rng = random.Random("%s-%s" % (cid, today.date().isoformat()))
        t = start
        while t < end:
            mins = rng.choice(DURATIONS_MIN)
            stop = t + datetime.timedelta(minutes=mins)
            title = rng.choice(TITLES)
            episode = rng.choice(EPISODES)
            lines.append('  <programme start="%s" stop="%s" channel="%s">'
                         % (fmt(t), fmt(stop), escape(cid)))
            lines.append('    <title lang="en">%s</title>' % escape(title))
            if episode:
                lines.append('    <sub-title lang="en">%s</sub-title>' % escape(episode))
            lines.append('    <desc lang="en">%s. A made-up programme for testing the guide.</desc>'
                         % escape(title))
            lines.append('  </programme>')
            t = stop
    lines.append('</tv>')
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d channels, %d days)" % (out, len(CHANNELS), days + 1))


if __name__ == "__main__":
    main()
