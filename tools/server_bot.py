#!/usr/bin/env python3
"""tldserver's launcher and Discord status (Karol 13.11: "pamiętaj o tym bocie na discorda").

Put this file beside tldserver.exe and start it instead of the exe (start_server.bat does):
  - it runs tldserver.exe in the same window (type the server's commands there as before: players, say, stop...);
  - if the server crashes it starts it again (not after `stop`, nor when it can't start at all);
  - with a Discord webhook (discord_webhook.txt beside it, one line: the webhook's URL) it keeps ONE status message
    up to date in that channel (online, who is in and where, the world's clock) and posts a short line when someone
    joins or leaves and when the server goes down or comes back.
Creating the webhook: Discord -> the channel's settings (gear) -> Integrations -> Webhooks -> New Webhook -> Copy
Webhook URL, then paste it into discord_webhook.txt. Keep that URL to yourselves: anyone with it can post there.

It also starts, when discord_bot_token.txt is beside it, the chat bot (discord_chat.py: answers on #chat with a local
language model) and the Discord bridge (tldcoop-discord\\local\\run_bridge.py, when that repository is cloned beside
it), and starts them again if they end.

Standard library only (Python 3.8+). Nothing here prints the webhook's URL or the bot's token.
"""
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, 'tldserver.exe')
STATUS = os.path.join(HERE, 'status.json')
HOOK_FILE = os.path.join(HERE, 'discord_webhook.txt')
STATE = os.path.join(HERE, 'bot_state.json')
LOG = os.path.join(HERE, 'server_bot.log')
CHAT_BOT = os.path.join(HERE, 'discord_chat.py')
CHAT_TOKEN = os.path.join(HERE, 'discord_bot_token.txt')
CHAT_LOG = os.path.join(HERE, 'discord_chat.log')
# The Discord bridge (the repository tldcoop-discord cloned beside us): its loop reads the channels and posts for the
# Claude chats, here instead of on GitHub Actions.
BRIDGE = os.path.join(HERE, 'tldcoop-discord', 'local', 'run_bridge.py')
BRIDGE_LOG = os.path.join(HERE, 'discord_bridge.log')

EDIT_EVERY_S = 60       # the status message at least this often (and at once when the players change)
QUICK_FAIL_S = 30       # a server that ends sooner than this after its start didn't really run
MAX_QUICK_FAILS = 3     # ...this many times in a row: something is wrong (the port, the world file): give up
RESTART_WAIT_S = 5


def log(text):
    line = time.strftime('[%Y-%m-%d %H:%M:%S] ') + '[bot] ' + text
    print(line, flush=True)
    try:
        with open(LOG, 'a', encoding='utf-8') as f:
            f.write(line + '\n')
    except OSError:
        pass


def load_json(path, default):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(data, f)
    os.replace(tmp, path)


class Discord:
    """One webhook: post lines, and keep one message (the status) edited."""

    def __init__(self):
        self.url = None
        try:
            with open(HOOK_FILE, encoding='utf-8') as f:
                url = f.read().strip()
        except OSError:
            url = ''
        if url.startswith(('https://discord.com/api/webhooks/', 'https://discordapp.com/api/webhooks/',
                           'https://canary.discord.com/api/webhooks/', 'https://ptb.discord.com/api/webhooks/')):
            self.url = url.split('?')[0].rstrip('/')
            log('discord: webhook found; status and join / leave lines go there')
        elif url:
            log('discord: discord_webhook.txt holds no Discord webhook URL; nothing goes to Discord')
        else:
            log('discord: no discord_webhook.txt beside the bot; nothing goes to Discord')
        self.state = load_json(STATE, {})
        self.failures = 0

    def call(self, method, path, body):
        data = json.dumps(body).encode('utf-8')
        req = urllib.request.Request(self.url + path, data=data, method=method,
                                     headers={'Content-Type': 'application/json', 'User-Agent': 'tldserver-bot (1.0)'})
        for _ in range(3):
            try:
                with urllib.request.urlopen(req, timeout=20) as r:
                    out = r.read()
                    self.failures = 0
                    return json.loads(out) if out else {}
            except urllib.error.HTTPError as e:
                if e.code == 429:
                    try:
                        wait = float(json.loads(e.read() or b'{}').get('retry_after', 2))
                    except ValueError:
                        wait = 2
                    time.sleep(min(wait, 30) + 0.5)
                    continue
                if e.code == 404 and '/messages/' in path:
                    return None  # (our status message was deleted: a new one is posted)
                self.failures += 1
                if self.failures <= 3:
                    log('discord: %s -> HTTP %d' % (method, e.code))
                return False
            except (urllib.error.URLError, OSError) as e:
                self.failures += 1
                if self.failures <= 3:
                    log('discord: no connection (%s)' % getattr(e, 'reason', e))
                return False
        return False

    def post(self, text):
        if self.url:
            self.call('POST', '?wait=true', {'content': text[:1900], 'allowed_mentions': {'parse': []}})

    def status(self, text):
        if not self.url:
            return
        mid = self.state.get('status_message')
        if mid:
            r = self.call('PATCH', '/messages/%s' % mid, {'content': text[:1900], 'allowed_mentions': {'parse': []}})
            if r is not False and r is not None:
                return
            if r is False:
                return  # (no connection: tried again next time)
        r = self.call('POST', '?wait=true', {'content': text[:1900], 'allowed_mentions': {'parse': []}})
        if r and r.get('id'):
            self.state['status_message'] = r['id']
            save_json(STATE, self.state)


class ChatBot:
    """A Python helper beside us (discord_chat.py; the Discord bridge's local/run_bridge.py), as a child with its own
    log; started again when it ends (not when it can't start: exit 2 = no token / refused, 3 = no discord.py; nor
    after 3 quick ends in a row)."""

    def __init__(self, name='chat bot', script=None, out_log=None):
        self.name = name
        self.script = script or CHAT_BOT
        self.out_log = out_log or CHAT_LOG
        self.proc = None
        self.started = 0
        self.quick = 0
        self.next_try = 0
        self.given_up = False
        self.enabled = os.path.isfile(self.script) and os.path.isfile(CHAT_TOKEN)
        if os.path.isfile(self.script) and not os.path.isfile(CHAT_TOKEN):
            log('%s: no discord_bot_token.txt: not started' % self.name)

    def tick(self):
        if not self.enabled or self.given_up:
            return
        if self.proc is not None:
            code = self.proc.poll()
            if code is None:
                return
            ran = time.time() - self.started
            self.proc = None
            if code in (2, 3):
                log('%s: ended (exit %d): see its log; not started again' % (self.name, code))
                self.given_up = True
                return
            self.quick = self.quick + 1 if ran < QUICK_FAIL_S else 0
            if self.quick >= MAX_QUICK_FAILS:
                log('%s: ended %d times right after starting: given up; see its log' % (self.name, self.quick))
                self.given_up = True
                return
            log('%s: ended (exit %s, after %d s): starting it again' % (self.name, code, ran))
            self.next_try = time.time() + RESTART_WAIT_S
        if time.time() < self.next_try:
            return
        try:
            out = open(self.out_log, 'a', encoding='utf-8')
            self.proc = subprocess.Popen([sys.executable, self.script], cwd=os.path.dirname(self.script), stdout=out, stderr=subprocess.STDOUT)
            self.started = time.time()
            log('%s: started (%s)' % (self.name, os.path.basename(self.script)))
        except OSError as e:
            log('%s: cannot start (%s)' % (self.name, e))
            self.given_up = True

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


class Helpers:
    def __init__(self, helpers):
        self.helpers = helpers

    def tick(self):
        for h in self.helpers:
            h.tick()

    def stop(self):
        for h in self.helpers:
            h.stop()


def stopped_on_purpose(since):
    """The server writes status.json with online false as it stops (stop, Ctrl+C, the window closed). Killed from
    outside (an antivirus, the task manager) it can't, and its exit code may still read 0."""
    st = load_json(STATUS, None)
    return bool(st) and st.get('online') is False and float(st.get('time', 0)) >= since - 1


def status_text(st, online):
    stamp = time.strftime('%d.%m %H:%M')
    if not online or not st.get('online'):
        return '🔴 **Serwer wyłączony**\n_stan z %s_' % stamp
    players = st.get('players') or []
    lines = ['🟢 **Serwer działa** (%s, gra %s)' % (st.get('name', 'Serwer'), st.get('game_version', '?'))]
    lines.append('Gracze: %d/%d' % (len(players), st.get('max', 3)))
    for p in players:
        lines.append('• %s%s' % (p.get('name', '?'), (' — ' + p['scene']) if p.get('scene') else ' — ładuje się'))
    if st.get('clock'):
        lines.append('Godzina w świecie: %s' % st['clock'])
    up = int(st.get('up_s', 0))
    lines.append('_działa od %dh %02dmin, stan z %s_' % (up // 3600, up % 3600 // 60, stamp))
    return '\n'.join(lines)


def watch(proc, discord, chat):
    """While the server runs: the status message and join / leave lines. Returns its exit code."""
    known = None
    last_edit = 0
    last_mtime = 0
    while True:
        chat.tick()
        code = proc.poll()
        if code is not None:
            return code
        time.sleep(2)
        try:
            mtime = os.path.getmtime(STATUS)
        except OSError:
            continue
        if mtime == last_mtime and time.time() - last_edit < EDIT_EVERY_S:
            continue
        last_mtime = mtime
        st = load_json(STATUS, None)
        if not st:
            continue
        names = {p.get('name', '?') for p in st.get('players') or []}
        changed = known is not None and names != known
        if known is not None:
            total = '%d/%d' % (len(names), st.get('max', 3))
            for n in sorted(names - known):
                discord.post('➕ **%s** wszedł na serwer (%s)' % (n, total))
            for n in sorted(known - names):
                discord.post('➖ **%s** wyszedł z serwera (%s)' % (n, total))
        known = names
        if changed or time.time() - last_edit >= EDIT_EVERY_S:
            discord.status(status_text(st, True))
            last_edit = time.time()


def main():
    if not os.path.isfile(EXE):
        log('no tldserver.exe beside the bot (%s)' % HERE)
        input('Enter zamyka okno...')
        return 1
    discord = Discord()
    chat = Helpers([ChatBot(), ChatBot('discord bridge', BRIDGE, BRIDGE_LOG)])
    try:
        return run(discord, chat)
    finally:
        chat.stop()


def run(discord, chat):
    quick_fails = 0
    first = True
    while True:
        log('starting tldserver.exe')
        started = time.time()
        proc = subprocess.Popen([EXE] + sys.argv[1:], cwd=HERE)
        if first:
            discord.post('🟢 Serwer wystartował')
        first = False
        try:
            code = watch(proc, discord, chat)
        except KeyboardInterrupt:  # (Ctrl+C reaches the server too: it saves and stops)
            code = proc.wait()
        ran = time.time() - started
        discord.status(status_text({}, False))
        if code == 0 and not stopped_on_purpose(started):
            log('tldserver.exe vanished without stopping (killed from outside? an antivirus?): treated as a crash')
            code = -1
        if code == 0:
            log('tldserver.exe stopped (exit 0): not started again')
            discord.post('🔴 Serwer wyłączony')
            return 0
        if ran < QUICK_FAIL_S:
            quick_fails += 1
            if quick_fails >= MAX_QUICK_FAILS:
                log('tldserver.exe ended %d times right after starting (exit %d): given up; see tldserver.log' % (quick_fails, code))
                discord.post('⚠️ Serwer nie może wystartować (szczegóły w tldserver.log na serwerze)')
                input('Enter zamyka okno...')
                return 1
        else:
            quick_fails = 0
        log('tldserver.exe ended (exit %d, after %d s): starting it again in %d s' % (code, ran, RESTART_WAIT_S))
        discord.post('⚠️ Serwer padł; uruchamiam go ponownie')
        time.sleep(RESTART_WAIT_S)


if __name__ == '__main__':
    sys.exit(main())
