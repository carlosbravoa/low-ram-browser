"""Check what sites learn about lrb: Chromium's client hints, the system's
languages, no prerendering.

  python3 test_identity.py <path to lrb>

content_shell said brand "content_shell" on platform "Unknown" and always
"en-us,en". Runs lrb with LANG=es_CL.UTF-8 on a local page.
"""

import os
import sys
import tempfile

from lrb_harness import cdp as cdplib
from lrb_harness import run as runlib


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="identity-", dir=runlib.ROOT + "/.profiles")
    log = os.path.join(work, "log")
    os.environ["LANG"] = "es_CL.UTF-8"
    os.environ.pop("LC_ALL", None)
    os.environ.pop("LANGUAGE", None)
    page = "file://" + os.path.join(runlib.PHASE0, "pages", "article.html")
    proc = runlib.launch([binary, f"--user-data-dir={work}/p", "--ozone-platform=headless",
                          "--remote-debugging-port=0", page], log, None, False)
    failures = 0

    def check(name, ok, detail=""):
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f": {detail}" if detail else ""))

    try:
        client = cdplib.CDP(runlib.wait_for_devtools(proc, log))
        session = client.attach_first_page()
        runlib.wait_ready(client, session, page, 20)

        def run(expression):
            reply = client.call("Runtime.evaluate", {
                "expression": expression, "returnByValue": True}, session)
            return reply["result"].get("value")

        brands = run("navigator.userAgentData.brands.map(b => b.brand)")
        check("client hints name Chromium", "Chromium" in brands and
              "content_shell" not in brands, brands)
        platform = run("navigator.userAgentData.platform")
        check("client hints say Linux", platform == "Linux", platform)
        languages = run("navigator.languages")
        check("languages follow the system (LANG=es_CL)",
              languages[:2] == ["es-CL", "es"], languages)
        prerender = run("HTMLScriptElement.supports('speculationrules')")
        check("speculation rules parse (prerendering is refused, not the API)",
              prerender is True, prerender)
    finally:
        runlib.stop(proc, None)
    print(f"{failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
