#!/usr/bin/env python3
"""Registers a GolfTracker round in GolfBox (Norway) by filling in "Innlever score".

GolfBox has no public API, so this drives the website like a person would:
log in, open Score -> Innlever score, pick date/course/tee/marker, type the
strokes per hole and press "Lagre". Run by .github/workflows/golfbox.yml.

Environment:
  ROUND_JSON        {"id", "course", "par": [...], "strokes": [...], "first"} from the display
  GOLFBOX_USERNAME  / GOLFBOX_PASSWORD   (GitHub secrets)
  GOLFBOX_MARKER    marker's name as shown in GolfBox (searched by Medlemsnavn)
  PLAY_DATE         dd.mm.yyyy, default today in Norway
  PLAY_TIME         HH:MM, default about four hours before now
  DRY_RUN           anything but "false" fills the form without pressing Lagre
  SCREENSHOT_DIR    where step screenshots go (uploaded only from private repos)
  PLAYWRIGHT_CHANNEL  "chrome" to use the installed Google Chrome instead of Playwright's
  GOLFBOX_FORM_URL  testing only: open this page instead of logging in
"""

import datetime
import json
import os
import re
import sys
import time
from pathlib import Path
from zoneinfo import ZoneInfo

from playwright.sync_api import Page, sync_playwright

LOGIN_URL = "https://www.golfbox.no/login.asp?lcid=1044"
LOGIN_APP_URL = "https://golfbox.golf/"
OSLO = ZoneInfo("Europe/Oslo")
HERE = Path(__file__).resolve().parent
SHOTS = Path(os.environ.get("SCREENSHOT_DIR", "screenshots"))
DRY_RUN = os.environ.get("DRY_RUN", "true").strip().lower() != "false"
TIMEOUT_MS = 20_000


class Fail(Exception):
    """A problem with a clear message for the workflow log."""


_step = 0


def log(msg: str) -> None:
    print(msg, flush=True)


def shot(page: Page, name: str) -> None:
    global _step
    _step += 1
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        page.screenshot(path=str(SHOTS / f"{_step:02d}-{name}.png"), full_page=True)
    except Exception as e:  # screenshots are best effort
        log(f"  (screenshot failed: {e})")


def env(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise Fail(f"{name} is not set")
    return value


def norm(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip().lower()


def xpath_literal(s: str) -> str:
    if "'" not in s:
        return f"'{s}'"
    return "concat('" + "', \"'\", '".join(s.split("'")) + "')"


# ---- Input ----------------------------------------------------------------------


def load_round() -> dict:
    try:
        r = json.loads(env("ROUND_JSON"))
    except json.JSONDecodeError as e:
        raise Fail(f"ROUND_JSON is not valid JSON: {e}")
    par, strokes = r.get("par"), r.get("strokes")
    if not isinstance(par, list) or not isinstance(strokes, list) or not 1 <= len(par) <= 18 or len(par) != len(strokes):
        raise Fail("ROUND_JSON needs 'par' and 'strokes' lists of the same length (1-18)")
    if not all(isinstance(x, int) and 0 <= x <= 20 for x in par + strokes):
        raise Fail("ROUND_JSON pars/strokes must be small whole numbers")
    if not any(strokes):
        raise Fail("The round has no strokes")
    r["first"] = int(r.get("first") or 1)
    r["course"] = str(r.get("course") or "")
    return r


def course_config(name: str) -> tuple[str, dict]:
    base = re.sub(r"\s*\((front 9|back 9|18)\)\s*$", "", name).strip()
    table = json.loads((HERE / "courses.json").read_text(encoding="utf-8"))
    for key, cfg in table.items():
        if not key.startswith("_") and norm(key) == norm(base):
            return base, cfg
    raise Fail(f'Course "{base}" is not in golfbox/courses.json - add it there')


def play_date_time() -> tuple[str, str]:
    now = datetime.datetime.now(OSLO)
    date = os.environ.get("PLAY_DATE", "").strip() or now.strftime("%d.%m.%Y")
    tee = os.environ.get("PLAY_TIME", "").strip()
    if not tee:
        # The display has no clock; rounds are sent right after finishing, so
        # assume the round started about four hours ago (not before 06:00).
        start = now - datetime.timedelta(hours=4)
        if start.date() != now.date() or start.hour < 6:
            start = now.replace(hour=6, minute=0)
        start = start.replace(minute=start.minute - start.minute % 10)
        tee = start.strftime("%H:%M")
    if not re.fullmatch(r"\d{2}\.\d{2}\.\d{4}", date):
        raise Fail(f"PLAY_DATE must be dd.mm.yyyy, got {date!r}")
    if not re.fullmatch(r"\d{2}:\d{2}", tee):
        raise Fail(f"PLAY_TIME must be HH:MM, got {tee!r}")
    return date, tee


# ---- Page helpers ---------------------------------------------------------------


def click_text_if_present(page: Page, text: str) -> bool:
    loc = page.get_by_text(text, exact=True)
    if loc.count() and loc.first.is_visible():
        loc.first.click()
        return True
    return False


def settle(page: Page) -> None:
    try:
        page.wait_for_load_state("networkidle", timeout=TIMEOUT_MS)
    except Exception:
        pass


def after_label(page: Page, label: str, tag: str):
    """First <tag> after the element whose own text is exactly `label`."""
    return page.locator(f"xpath=(//*[normalize-space(text())={xpath_literal(label)}])[1]/following::{tag}[1]")


def options_of(select) -> list[dict]:
    return select.evaluate(
        "s => [...s.options].map(o => ({value: o.value, text: o.textContent.trim(), disabled: o.disabled}))"
    )


def pick(options: list[dict], want: str, prefer: str | None = None) -> dict | None:
    w = norm(want)
    # Match on text only: GolfBox uses values like "0" for real choices (e.g. Medlemsnavn),
    # and placeholders such as "Velg bane" never match a wanted text anyway.
    usable = [o for o in options if not o["disabled"]] or options
    for test in (lambda t: t == w, lambda t: t.startswith(w), lambda t: w in t):
        hits = [o for o in usable if test(norm(o["text"]))]
        if hits:
            if prefer:
                preferred = [o for o in hits if norm(prefer) in norm(o["text"])]
                if preferred:
                    return preferred[0]
            return hits[0]
    return None


def choose(page: Page, label: str, want: str | None, prefer: str | None = None) -> None:
    """Selects the option matching `want` in the dropdown under `label`.

    Dropdowns further down reload after an earlier choice, so this waits for a
    matching option to appear. want=None keeps the current choice."""
    select = after_label(page, label, "select")
    select.wait_for(state="attached", timeout=TIMEOUT_MS)
    if want is None:
        log(f"  {label}: keeping '{selected_text(select)}'")
        return
    deadline = time.monotonic() + TIMEOUT_MS / 1000
    while True:
        opts = options_of(select)
        match = pick(opts, want, prefer)
        if match or time.monotonic() > deadline:
            break
        time.sleep(0.5)
    if not match:
        texts = ", ".join(o["text"] for o in opts if o["text"]) or "(none)"
        raise Fail(f'"{want}" not found in {label}. GolfBox offers: {texts}')
    if select.input_value() == match["value"]:
        log(f"  {label}: '{match['text']}' (already selected)")
        return
    if select.is_disabled():
        raise Fail(f"{label} is locked to '{selected_text(select)}', wanted '{want}'")
    select.select_option(value=match["value"])
    log(f"  {label}: '{match['text']}'")
    settle(page)


def selected_text(select) -> str:
    return select.evaluate("s => s.options[s.selectedIndex] ? s.options[s.selectedIndex].textContent.trim() : ''")


def type_into(locator, value: str) -> None:
    locator.click()
    locator.fill(value)
    locator.press("Tab")


# ---- Steps ----------------------------------------------------------------------


def login(page: Page, username: str, password: str) -> None:
    log("Logging in")
    # Same request GolfBox's own login page (golfbox.golf) makes for Norway.
    page.goto(LOGIN_APP_URL, wait_until="domcontentloaded")
    with page.expect_navigation(timeout=TIMEOUT_MS * 2):
        page.evaluate(
            """([url, user, pass]) => {
                const f = document.createElement('form');
                f.method = 'post'; f.action = url; f.style.display = 'none';
                const add = (n, v) => { const i = document.createElement('input');
                    i.type = 'hidden'; i.name = n; i.value = v; f.appendChild(i); };
                add('loginform.submitted', 'true'); add('command', 'login');
                add('loginform.username', user); add('loginform.password', pass);
                add('loginform.submit', 'LOGIN');
                document.body.appendChild(f); f.submit();
            }""",
            [LOGIN_URL, username, password],
        )
    settle(page)
    # A failed login lands on GolfBox's "Logg inn / Glemt passord" page with an orange message box.
    if page.get_by_text("Glemt passord", exact=True).count() or not page.get_by_text("Min forside").count():
        shot(page, "login-failed")
        message = ""
        for text in ("Bruker ikke funnet", "Feil", "passord"):
            hit = page.get_by_text(text).first
            if hit.count() and hit.is_visible():
                message = hit.inner_text().strip().splitlines()[0]
                break
        raise Fail("Login failed" + (f" - GolfBox says: {message}" if message else "")
                   + ". Check the GOLFBOX_USERNAME / GOLFBOX_PASSWORD secrets.")


def dismiss_popups(page: Page) -> None:
    if click_text_if_present(page, "Aksepter Nødvendige"):
        log("  Cookie banner: accepted necessary cookies")
    if page.get_by_text("Oppdatering av kontaktinformasjon").count():
        log("  Contact info check: confirming 'Ja, alt er korrekt'")
        page.get_by_text("Ja, alt er korrekt", exact=True).click()
        page.get_by_text("OK", exact=True).click()
        settle(page)
        click_text_if_present(page, "Aksepter Nødvendige")


def open_score_form(page: Page) -> None:
    log("Opening Score -> Innlever score")
    link = page.get_by_text("Innlever score", exact=True)
    if not (link.count() and link.first.is_visible()):
        page.get_by_text("Score", exact=True).first.click()
        settle(page)
    page.get_by_text("Innlever score", exact=True).first.click()
    settle(page)
    dismiss_popups(page)
    page.get_by_text("Spilledato", exact=True).wait_for(timeout=TIMEOUT_MS)
    log(f"  Form at {page.url}")


def fill_round_info(page: Page, r: dict, cfg: dict, date: str, tee_time: str) -> None:
    log("Filling round info")
    type_into(after_label(page, "Spilledato", "input"), date)
    type_into(after_label(page, "Spilletidspunkt", "input"), tee_time)
    log(f"  Date {date} {tee_time}")
    holes = len(r["par"])
    choose(page, "Hull", f"{holes} hull", prefer="10" if holes == 9 and r["first"] == 10 else None)
    choose(page, "Land", cfg.get("country"))
    choose(page, "Klubb", cfg.get("club"))
    choose(page, "Bane", cfg.get("course"))
    choose(page, "Utslagssted", cfg.get("tee"))


def set_marker(page: Page, marker: str) -> None:
    log("Selecting marker")
    mode = after_label(page, "Markør", "select")
    mode.wait_for(state="attached", timeout=TIMEOUT_MS)
    opts = options_of(mode)
    by_name = pick(opts, "Medlemsnavn") or pick(opts, "navn")
    if not by_name:
        raise Fail("Marker search has no 'Medlemsnavn' option. Offers: " + ", ".join(o["text"] for o in opts))
    mode.select_option(value=by_name["value"])
    settle(page)
    search = mode.locator(
        "xpath=following::input[(@type='text' or @type='search' or not(@type)) and not(@disabled)][1]"
    )
    # Type key by key: GolfBox only enables "Søk" on real keystrokes, not a pasted value.
    search.click()
    search.fill("")
    search.press_sequentially(marker, delay=40)
    # "Søk" is text inside the real button element; click that, else press Enter.
    button = page.locator(
        "xpath=(//*[normalize-space(text())='Søk'])[1]/ancestor-or-self::*"
        "[self::button or self::a or @role='button' or contains(concat(' ', @class, ' '), ' btn ')][1]"
    )
    try:
        button.first.click(timeout=5000)
    except Exception:
        log("  Søk button not clickable, pressing Enter in the search field")
        search.press("Enter")
    row = page.locator(f"xpath=//tr[td[contains(normalize-space(.), {xpath_literal(marker)})]]")
    try:
        row.first.wait_for(timeout=TIMEOUT_MS)
    except Exception:
        shot(page, "marker-not-found")
        raise Fail(f"Marker '{marker}' not found in GolfBox search")
    if row.count() > 1:
        log(f"  {row.count()} matches, picking the first")
    row.first.click()
    settle(page)
    log("  Marker selected")


def fill_scores(page: Page, r: dict) -> None:
    log("Entering hole scores")
    first_hole = r["first"]

    def hole_input(n: int):
        return page.locator(f"xpath=//tr[normalize-space(td[1])='{n}'][.//input]//input[not(@type='hidden')]").first

    if not hole_input(first_hole).is_visible():
        page.get_by_text("Tast inn hullscorer", exact=True).click()
        settle(page)
    for i, strokes in enumerate(r["strokes"]):
        n = first_hole + i
        if not strokes:
            log(f"  Hole {n}: not played, left empty")
            continue
        field = hole_input(n)
        if not field.count():
            raise Fail(f"No stroke field for hole {n} on the scorecard")
        type_into(field, str(strokes))
    total = sum(r["strokes"])
    cell = page.locator("xpath=//tr[normalize-space(td[1])='TOTAL']/td[6]")
    shown = cell.first.inner_text().strip() if cell.count() else "?"
    log(f"  Entered {sum(1 for s in r['strokes'] if s)} holes, total {total} (GolfBox shows {shown})")
    if shown.isdigit() and int(shown) != total:
        log(f"::warning::GolfBox shows total {shown}, the round total is {total} - check the screenshots")


def save(page: Page) -> None:
    if DRY_RUN:
        log("DRY RUN: form filled, not pressing Lagre (set repository variable GOLFBOX_DRY_RUN=false to submit)")
        return
    log("Pressing Lagre")
    page.get_by_text("Lagre", exact=True).first.click()
    settle(page)
    shot(page, "after-save")
    errors = page.locator(".errorbox:visible, .alert-danger:visible, .error:visible, [role=alert]:visible")
    texts = [t.strip() for t in errors.all_inner_texts() if t.strip()]
    if texts:
        raise Fail("GolfBox reported: " + " | ".join(texts))
    log("Saved. Your marker now has to approve it in GolfBox (Score -> Til godkjennelse).")


def main() -> int:
    r = load_round()
    base, cfg = course_config(r["course"])
    date, tee_time = play_date_time()
    marker = env("GOLFBOX_MARKER")
    log(f"Round #{r.get('id', '?')}: {r['course']}, {sum(r['strokes'])} strokes, {len(r['par'])} holes"
        f"{' (dry run)' if DRY_RUN else ''}")

    with sync_playwright() as pw:
        # PLAYWRIGHT_CHANNEL=chrome uses the installed Google Chrome (as on GitHub's
        # runners) instead of a browser downloaded by "playwright install".
        browser = pw.chromium.launch(
            headless=os.environ.get("HEADFUL") != "1",
            channel=os.environ.get("PLAYWRIGHT_CHANNEL") or None,
        )
        context = browser.new_context(
            locale="nb-NO",
            timezone_id="Europe/Oslo",
            viewport={"width": 1280, "height": 900},
            user_agent=(
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
            ),
        )
        page = context.new_page()
        page.set_default_timeout(TIMEOUT_MS)
        try:
            form_url = os.environ.get("GOLFBOX_FORM_URL")
            if form_url:
                page.goto(form_url)
            else:
                login(page, env("GOLFBOX_USERNAME"), env("GOLFBOX_PASSWORD"))
                dismiss_popups(page)
                shot(page, "logged-in")
                open_score_form(page)
            fill_round_info(page, r, cfg, date, tee_time)
            shot(page, "round-info")
            set_marker(page, marker)
            fill_scores(page, r)
            shot(page, "filled")
            save(page)
        except Exception:
            shot(page, "error")
            raise
        finally:
            browser.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(f"::error::{e}", flush=True)
        sys.exit(1)
    except Exception as e:
        # Unexpected crash (e.g. a page element never appeared): show the gist as an
        # annotation on the run page as well as the full traceback in the log.
        import traceback

        traceback.print_exc()
        lines = [ln.strip() for ln in str(e).splitlines() if ln.strip()][:10]
        print(f"::error::{type(e).__name__}: {' | '.join(lines)}", flush=True)
        sys.exit(1)
