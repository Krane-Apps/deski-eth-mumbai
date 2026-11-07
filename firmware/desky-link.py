# desky-link: shows desky's serial log on the laptop and opens the links desky asks for (ens names) in the browser.
# run:  uv run --with pyserial firmware/desky-link.py            (finds desky's usb port)
#       uv run --with pyserial firmware/desky-link.py /dev/cu.usbmodemXXXX
# check: python3 firmware/desky-link.py --selftest
# opening the port restarts desky. close this before you flash new firmware.
import glob
import re
import sys
import time
import webbrowser

OPEN_LINE = re.compile(r"\[OPEN\] (\S+)")
# plain https links only: no quotes, backslashes or spaces reach the browser launcher
SAFE_URL = re.compile(r"https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~/%?=&#-]*)?")


def link(line: str) -> str | None:
    m = OPEN_LINE.search(line)
    return m.group(1) if m and SAFE_URL.fullmatch(m.group(1)) else None


def selftest() -> None:
    assert link("[ 1200 ms] [OPEN] https://vitalik.eth.limo") == "https://vitalik.eth.limo"
    assert link("[ 1200 ms] [OPEN] file:///etc/passwd") is None
    assert link('[ 1200 ms] [OPEN] https://x.limo/"&say') is None
    assert link("[ 1200 ms] [CHAT] desky says: https://vitalik.eth.limo") is None
    print("desky-link selftest ok")


def find_port() -> str | None:
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None


def main() -> None:
    if sys.argv[1:] == ["--selftest"]:
        return selftest()
    import serial  # pyserial

    while True:  # keeps going when desky is unplugged or restarts
        port = sys.argv[1] if len(sys.argv) > 1 else find_port()
        if not port:
            print("waiting for desky on usb...", flush=True)
            time.sleep(2)
            continue
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, 115200, 1
            s.dtr = s.rts = False  # so opening the port does not hold desky in reset
            s.open()
            print(f"listening to desky on {port}. Links desky finds open in your browser.", flush=True)
            while True:
                line = s.readline().decode(errors="ignore").strip()
                if not line:
                    continue
                print(line, flush=True)
                url = link(line)
                if url:
                    webbrowser.open(url)
                    print(f"  -> opened {url} in your browser", flush=True)
        except serial.SerialException as err:
            print(f"lost desky ({err}), trying again...", flush=True)
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
