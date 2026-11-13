#!/usr/bin/env python3
"""The server's Discord chat bot (Karol 13.11: "żeby też czytał #chat i jakiś lokalny model językowy który będzie
odpowiadać na oznaczenie tego bota w sposób wulgarny i bardzo bezpośredni, dissując i korzystając z polskich memów
oraz emotek").

It reads the #chat channel (the last messages, so it knows what is going on) and answers when someone mentions it
(@bot) or replies to one of its messages. The answers come from a LOCAL language model through Ollama on this PC
(nothing leaves the PC but Discord itself). It also knows the server's state (status.json: who is in, the clock).

Setup on the server PC (README.md, "Bot czatu"):
  1. Ollama: winget install Ollama.Ollama, then:  ollama pull SpeakLeash/bielik-minitron-7B-v3.0-instruct:Q4_K_M
  2. Python's Discord library:  py -3 -m pip install discord.py
  3. A Discord bot (not the webhook): discord.com/developers/applications -> New Application -> Bot -> Reset Token
     -> copy it into discord_bot_token.txt beside this file; on the same page switch on MESSAGE CONTENT INTENT; then
     OAuth2 -> URL Generator: scopes "bot", permissions View Channels, Send Messages, Read Message History -> open
     the link and add the bot to your server.
  4. Nothing else: server_bot.py starts this script by itself when discord_bot_token.txt is there.
Keep the token to yourselves: whoever has it controls the bot. Nothing here prints it.

Optional discord_chat.json beside it overrides the defaults below, e.g. {"model": "...", "channels": ["chat"]}.
"""
import asyncio
import collections
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TOKEN_FILE = os.path.join(HERE, 'discord_bot_token.txt')
CONF_FILE = os.path.join(HERE, 'discord_chat.json')
STATUS = os.path.join(HERE, 'status.json')
LOG = os.path.join(HERE, 'discord_chat.log')

DEFAULTS = {
    'model': 'SpeakLeash/bielik-minitron-7B-v3.0-instruct:Q4_K_M',
    'ollama': 'http://127.0.0.1:11434',
    'channels': ['chat'],     # channel names it reads and answers in
    'history': 25,            # messages of a channel it keeps as context
    'cooldown_s': 6,          # per person, between answers
    'temperature': 0.95,
    'max_tokens': 280,        # an answer's length (a few sentences)
    'timeout_s': 120,         # the model's answer, at most
    'extra': '',              # more lines for the persona (inside jokes, nicknames...)
}

PERSONA = """Jesteś {name}, bot serwera co-op The Long Dark tej ekipy na Discordzie. Ludzie oznaczają cię, żeby pogadać, \
zapytać o serwer albo dostać zjebkę.

Twój styl:
- Gadasz jak ziomek z osiedla: wulgarnie, bez owijania w bawełnę, bezczelnie i krótko (1-4 zdania).
- Cisniesz po osobie, która cię oznaczyła: po tym, co napisała, jak gra, ile razy zjadł ją wilk, że zamarzła z \
zapałkami w kieszeni, że znowu nie ma czym rozpalić. Dissujesz z przymrużeniem oka, jak kumpel kumpla.
- Wrzucasz polskie memy i teksty, np.: "kto pytał", "ło panie", "Janusz", "Grażyna", "Sebix", "essa", "byczq", \
"mordo", "no i elegancko", "jest git", "Polska gurom", "co ty wiesz o zabijaniu", "a mówiłem", "mamy to", \
"nie no, szacun", "tak bardzo", "przypał". Nie wklejaj ich wszystkich naraz, tylko to, co pasuje.
- Dużo emotek: 💀🤡🗿🫵😂🔥🍺🥶🐺 itp.
- Piszesz po polsku. Nie tłumaczysz się, nie przepraszasz, nie piszesz, że jesteś modelem językowym.

Granice (zawsze): nie jedziesz po rasie, narodowości, religii, orientacji, niepełnosprawności, wyglądzie ciała; \
żadnych gróźb, żadnych tekstów seksualnych o prawdziwych ludziach, żadnych prawdziwych danych osobowych. \
Wulgaryzmy i roast - tak, nienawiść - nie.

Gdy ktoś pyta o coś konkretnego (np. kto gra, czy serwer działa, która godzina w grze), odpowiadasz prawdziwie, \
z tego co niżej, tylko swoim stylem. Nie zmyślasz stanu serwera.

{status}{extra}"""


def log(text):
    line = time.strftime('[%Y-%m-%d %H:%M:%S] ') + text
    print(line, flush=True)
    try:
        with open(LOG, 'a', encoding='utf-8') as f:
            f.write(line + '\n')
    except OSError:
        pass


def load_conf():
    conf = dict(DEFAULTS)
    try:
        with open(CONF_FILE, encoding='utf-8') as f:
            conf.update(json.load(f))
    except (OSError, ValueError):
        pass
    conf['channels'] = [str(c).lstrip('#').lower() for c in conf.get('channels') or ['chat']]
    return conf


def read_token():
    try:
        with open(TOKEN_FILE, encoding='utf-8-sig') as f:
            return f.read().strip().strip('"').strip()
    except OSError:
        return ''


def status_text():
    """The server as status.json says, for the persona."""
    try:
        with open(STATUS, encoding='utf-8') as f:
            st = json.load(f)
    except (OSError, ValueError):
        return 'Stan serwera: nieznany (brak status.json).\n'
    age = time.time() - float(st.get('time', 0))
    if not st.get('online') or age > 120:
        return 'Stan serwera: WYŁĄCZONY.\n'
    players = st.get('players') or []
    names = ', '.join('%s (%s)' % (p.get('name', '?'), p.get('scene') or 'ładuje się') for p in players)
    out = 'Stan serwera: działa, gracze %d/%d%s.\n' % (len(players), st.get('max', 3), (': ' + names) if names else ', nikt nie gra')
    if st.get('clock'):
        out += 'Godzina i pogoda w grze: %s.\n' % st['clock']
    return out


def build_messages(conf, bot_name, history, trigger_id):
    """System persona + the channel's last messages as a chat; the bot's own as 'assistant'. `history`: oldest first,
    entries (id, author, is_bot, text). Everything after the trigger is left out."""
    extra = ('\nDodatkowo o tej ekipie:\n' + conf['extra'].strip() + '\n') if conf.get('extra') else ''
    msgs = [{'role': 'system', 'content': PERSONA.format(name=bot_name, status=status_text(), extra=extra)}]
    for mid, author, is_bot, text in history:
        if is_bot:
            msgs.append({'role': 'assistant', 'content': text})
        else:
            line = '%s: %s' % (author, text)
            if msgs[-1]['role'] == 'user':  # (several people in a row: one turn, a line each)
                msgs[-1]['content'] += '\n' + line
            else:
                msgs.append({'role': 'user', 'content': line})
        if mid == trigger_id:
            break
    who = next((author for mid, author, is_bot, text in history if mid == trigger_id), None)
    if msgs[-1]['role'] != 'user':
        msgs.append({'role': 'user', 'content': '(ktoś cię oznaczył)'})
    if who:
        msgs[-1]['content'] += '\n[Oznaczył cię %s: odpowiedz teraz jemu.]' % who
    return msgs


def clean_answer(text, bot_name):
    """What the model said, fit for Discord: no "Bot:" prefix, no @everyone / @here, no role pings, 1900 chars."""
    text = (text or '').strip()
    text = re.sub(r'^\s*(%s|bot|assistant|asystent)\s*:\s*' % re.escape(bot_name), '', text, flags=re.I)
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.S).strip()
    text = text.replace('@everyone', '@​everyone').replace('@here', '@​here')
    text = re.sub(r'<@&\d+>', '', text)
    return text[:1900].strip()


def ask_model(conf, messages):
    """Ollama's /api/chat, not streamed. Returns the text, or None (no Ollama, no model, too slow)."""
    body = json.dumps({
        'model': conf['model'],
        'messages': messages,
        'stream': False,
        'keep_alive': '30m',
        'options': {'temperature': conf['temperature'], 'num_predict': conf['max_tokens'], 'num_ctx': 4096},
    }).encode('utf-8')
    req = urllib.request.Request(conf['ollama'].rstrip('/') + '/api/chat', data=body,
                                 headers={'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(req, timeout=conf['timeout_s']) as r:
            out = json.loads(r.read() or b'{}')
        return (out.get('message') or {}).get('content')
    except urllib.error.HTTPError as e:
        detail = e.read()[:300].decode('utf-8', 'replace')
        log('model: HTTP %d from Ollama (%s)' % (e.code, detail))
    except (urllib.error.URLError, OSError, ValueError) as e:
        log('model: no answer from Ollama at %s (%s)' % (conf['ollama'], getattr(e, 'reason', e)))
    return None


FALLBACKS = [
    'Mózg mi zamarzł 🥶 (model nie odpowiada; ktoś niech sprawdzi Ollamę na serwerze)',
    'Nie teraz, mordo, mi się procesor przegrzał 🔥💀 (model nie odpowiada)',
]


def main():
    try:
        import discord  # noqa: F401  (py -3 -m pip install discord.py)
    except ImportError:
        log('no discord.py: run  py -3 -m pip install discord.py  and start the server again')
        return 3
    token = read_token()
    if not token:
        log('no discord_bot_token.txt beside the bot: nothing to do')
        return 2
    conf = load_conf()
    log('starting: model %s, channels %s' % (conf['model'], ', '.join('#' + c for c in conf['channels'])))

    intents = discord.Intents.default()
    intents.message_content = True
    client = discord.Client(intents=intents, allowed_mentions=discord.AllowedMentions(everyone=False, roles=False, users=True))
    histories = collections.defaultdict(lambda: collections.deque(maxlen=int(conf['history'])))
    last_answer = {}
    busy = asyncio.Lock()  # one answer at a time: one GPU
    fallback_n = [0]

    def remember(message):
        text = message.clean_content.strip()
        if message.attachments:
            text += (' ' if text else '') + '[załącznik]'
        if not text:
            return
        author = message.author.display_name
        histories[message.channel.id].append((message.id, author, message.author == client.user, text[:600]))

    @client.event
    async def on_ready():
        log('logged in as %s; reading %s' % (client.user, ', '.join('#' + c for c in conf['channels'])))
        for guild in client.guilds:
            for ch in guild.text_channels:
                if ch.name.lower() not in conf['channels']:
                    continue
                try:
                    msgs = [m async for m in ch.history(limit=int(conf['history']))]
                except discord.DiscordException as e:
                    log('cannot read #%s in %s (%s): give the bot "Read Message History" there' % (ch.name, guild.name, e))
                    continue
                for m in reversed(msgs):
                    remember(m)
                log('#%s in %s: %d earlier message(s) read' % (ch.name, guild.name, len(msgs)))

    @client.event
    async def on_message(message):
        if not message.guild or getattr(message.channel, 'name', '').lower() not in conf['channels']:
            return
        remember(message)
        if message.author.bot:
            return
        mentioned = client.user in message.mentions
        replied = False
        ref = message.reference
        if ref and isinstance(ref.resolved, discord.Message):
            replied = ref.resolved.author == client.user
        if not mentioned and not replied:
            return
        now = time.time()
        if now - last_answer.get(message.author.id, 0) < conf['cooldown_s']:
            return
        last_answer[message.author.id] = now
        bot_name = message.guild.me.display_name if message.guild.me else client.user.name
        msgs = build_messages(conf, bot_name, list(histories[message.channel.id]), message.id)
        async with busy:
            async with message.channel.typing():
                started = time.time()
                text = await asyncio.to_thread(ask_model, conf, msgs)
        answer = clean_answer(text, bot_name) if text else ''
        if not answer:
            answer = FALLBACKS[fallback_n[0] % len(FALLBACKS)]
            fallback_n[0] += 1
        try:
            await message.reply(answer, mention_author=True)
            log('answered %s in #%s (%.1f s, %d chars)' % (message.author.display_name, message.channel.name,
                                                           time.time() - started, len(answer)))
        except discord.DiscordException as e:
            log('cannot answer in #%s (%s): give the bot "Send Messages" there' % (message.channel.name, e))

    try:
        client.run(token, log_handler=None)
    except discord.LoginFailure:
        log('Discord refused the token: copy it again (Bot -> Reset Token) into discord_bot_token.txt')
        return 2
    except discord.PrivilegedIntentsRequired:
        log('switch on MESSAGE CONTENT INTENT: discord.com/developers/applications -> the bot -> Bot -> Privileged '
            'Gateway Intents')
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
