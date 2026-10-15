"""Send scripted controller commands to a running mcla.exe (fork-only dev tool).

The game serves the commands when started with --mcla_pad_pipe=mcla-pad; see
src/dev_pad.cpp for the command list. Every command gets one reply line,
"ok ..." or "err ...".

  pad.py press start
  pad.py hold rt 3s
  pad.py stick lx -0.5 1s
  pad.py shot C:\\temp\\title.png 0.5
  pad.py state
  pad.py run script.pad      # send a script's lines one by one
  pad.py -i                  # interactive
"""

import argparse
import sys
import time

PIPE = r"\\.\pipe\{}"


def connect(name, timeout):
    deadline = time.monotonic() + timeout
    while True:
        try:
            return open(PIPE.format(name), "r+b", buffering=0)
        except OSError:
            if time.monotonic() > deadline:
                raise SystemExit(f"no game serving {PIPE.format(name)}")
            time.sleep(0.25)


def send(pipe, line):
    pipe.write((line.strip() + "\n").encode())
    reply = bytearray()
    while not reply.endswith(b"\n"):
        chunk = pipe.read(1)
        if not chunk:
            raise SystemExit("the game closed the pipe")
        reply += chunk
    return reply.decode().strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pipe", default="mcla-pad")
    parser.add_argument("--connect-timeout", type=float, default=30)
    parser.add_argument("-i", action="store_true", help="interactive")
    parser.add_argument("words", nargs="*")
    args = parser.parse_args()

    pipe = connect(args.pipe, args.connect_timeout)
    failed = False
    if args.i:
        for line in sys.stdin:
            print(send(pipe, line), flush=True)
    elif args.words[:1] == ["run"] and len(args.words) == 2:
        with open(args.words[1], encoding="utf-8") as script:
            for line in script:
                if not line.strip() or line.lstrip().startswith("#"):
                    continue
                reply = send(pipe, line)
                print(f"{line.strip()} -> {reply}", flush=True)
                failed |= reply.startswith("err")
    else:
        reply = send(pipe, " ".join(args.words))
        print(reply)
        failed = reply.startswith("err")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
