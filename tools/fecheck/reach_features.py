"""reach_features.py [--roadmap <csv>] [--out <dir>]: sorts the never-run ('reach') functions of
roadmap.csv by game feature, so the next recording scenarios can be picked by payoff.

Evidence per function, strongest first:
  1. its own name (runtime/decomp_symbols.csv) or source file, when they say something;
  2. the named functions around it in the image (same translation unit: the nearest named
     neighbours within 0x3000 bytes on both sides);
  3. the source file of its callers that ran (roadmap 'ran_callers').
Features are keyword rules on that text (FEATURES below, first match wins); a function no rule
matches stays 'other'. Writes <out>/reach_features.csv (va, size, feature, evidence) and prints
feature -> count, bytes, and how a recording reaches it (HOW).
"""
import argparse, bisect, collections, csv, os, re
import common

# (feature, regex over the evidence text); first match wins, so specific before general
FEATURES = [
    ('sound', r'\bsound_|music|ambience|dsound'),
    ('voice', r'voice|communicator|xhv|headset|talker'),
    ('live', r'xbox_live|xonline|online_|_online|friend|clan|matchmak|optimatch|quickmatch|presence|'
             r'arbitrat|live_message|message_send|bungie|playlist|stats_|session_search|title_storage'),
    ('net_session', r'network_|session|transport|simulation_|observer|membership|host_migrat|'
                    r'netdebug|peer|bandwidth|channel|replication|system_link|squad_session'),
    ('saved_game', r'saved_game|savegame|profile|storage|xcache|cache_file|campaign_save|checkpoint'),
    ('ui_screens', r'user_interface|ui_|_screen|window_manager|menu|dialog|widget|text_parser|'
                   r'attract|start_prompt|carnage|scoreboard'),
    ('splitscreen_coop', r'split|coop|local_player|player_count|controller'),
    ('hud', r'hud|motion_sensor|chud|nav_point|waypoint'),
    ('game_types', r'game_engine|slayer|ctf|oddball|koth|juggernaut|territor|assault|team_balanc|'
                   r'game_variant|statborg|game_results'),
    ('ai_campaign', r'\bai_|actor|squad|\bcs_|\bhs_|\bscript|cinematic|recorded_anim|scenario|encounter|'
                    r'swarm|flock|dialogue|objectives'),
    ('vehicles', r'vehicle|warthog|banshee|ghost|scorpion|wraith|spectre|boarding'),
    ('combat', r'damage|weapon|projectile|melee|grenade|explosion|equipment'),
    ('objects_units', r'object|unit|biped|device|machine|teleport|item|crate'),
    ('render_fx', r'render|rasteriz|texture|bitmap|shader|particle|effect|light|decal|weather|'
                  r'contrail|spherical|lens|sky|camera|screenshot'),
    ('physics', r'havok|physics|collision|hk|bsp|cluster'),
    ('engine', r'string|unicode|sorted_array|memory|data_|tag|cache|file|crc|random|math|vector'),
]

# how a recording reaches each feature (the rig scenario, or why none exists yet)
HOW = {
    'voice': 'no communicator emulated: needs a voice HLE (XHV) or a fake headset; not reachable today',
    'live': 'RECOMP_OPEN_LIVE=1 (local gamertag): rigs_cov.sh live = sign in, crawl Xbox Live menus '
            '(friends, clans, Optimatch/Quickmatch, custom games); two copies for party/match',
    'net_session': 'rigs_cov.sh drop<who>/leave scenarios: joiner or host quits / is killed mid-lobby '
                   'and mid-game (session leave, host migration, connection-lost dialogs)',
    'saved_game': 'rigs_cov.sh menus: Settings -> profile create/edit/delete, campaign Save and Quit/Resume',
    'ui_screens': 'rigs_cov.sh menus: blind A/B crawl of every front-end and pause-menu row, 2 levels deep',
    'splitscreen_coop': 'rigs_cov.sh coop<row>: RECOMP_USB_PADS=2, Campaign -> Cooperative, pad p2 joins',
    'hud': 'mostly co-op / multiplayer HUD states: coop and link scenarios',
    'ai_campaign': 'campaign levels/events not yet played to their end: rigs5.sh camp<row> with longer FIGHT_MIN',
    'vehicles': 'vehicle boarding/driving: campaign levels with vehicles (Outskirts, Metropolis, Delta Halo) or MP maps',
    'combat': 'more fighting: campaign/MP fight loops',
    'objects_units': 'more gameplay',
    'game_types': 'link batch (all 7 game types on two copies)',
    'render_fx': 'more maps/effects',
    'sound': 'audio paths muted in rigs (RECOMP_APU_MUTE=1); a run without mute',
    'physics': 'more gameplay',
    'engine': 'utility code: follows its callers',
    'other': 'no evidence: see scenarios.txt gateway/table groups',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--roadmap', default=os.path.join(common.WORK, 'roadmap', 'roadmap.csv'))
    ap.add_argument('--symbols', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                      '..', '..', 'runtime', 'decomp_symbols.csv'))
    ap.add_argument('--out', default=os.path.join(common.WORK, 'roadmap'))
    a = ap.parse_args()
    if not os.path.exists(a.symbols):   # a worktree without the untracked runtime/
        a.symbols = os.path.join(common.HALO2, '..', 'openhalo2', 'runtime', 'decomp_symbols.csv')
    names = {}
    for r in csv.DictReader(open(a.symbols, newline='')):
        if r['kind'] == 'function' and r['name'] and not re.match(r'^(function|sub|fn)_[0-9a-f]+$', r['name']):
            names[int(r['va'], 16)] = (r['name'], r.get('source', ''))
    srcof = {}
    rows = list(csv.DictReader(open(a.roadmap, newline='')))
    for r in rows:
        srcof[int(r['va'], 16)] = os.path.basename(r['source'] or '')
    nv = sorted(names)
    reach = [r for r in rows if r['category'] == 'reach']
    out = []
    for r in reach:
        va = int(r['va'], 16)
        ev = []
        if va in names:
            ev.append(names[va][0])
        s = os.path.basename(r['source'] or '')
        if s and not s.startswith('unknown_'):
            ev.append(s)
        i = bisect.bisect_left(nv, va)
        for j in (i - 1, i - 2, i, i + 1):
            if 0 <= j < len(nv) and abs(nv[j] - va) < 0x3000 and nv[j] != va:
                ev.append(names[nv[j]][0])
        for c in (r.get('ran_callers') or '').split():
            try:
                cs = srcof.get(int(c, 16), '')
            except ValueError:
                continue
            if cs and not cs.startswith('unknown_'):
                ev.append('caller:' + cs)
            if int(c, 16) in names:
                ev.append('caller:' + names[int(c, 16)][0])
        text = ' '.join(ev).lower()
        feat = 'other'
        for f, rx in FEATURES:
            if re.search(rx, text):
                feat = f
                break
        out.append((va, int(r['size'] or 0), feat, ' '.join(ev[:6])))
    with open(os.path.join(a.out, 'reach_features.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['va', 'size', 'feature', 'evidence'])
        for va, sz, ft, ev in out:
            w.writerow(['%08x' % va, sz, ft, ev])
    agg = collections.defaultdict(lambda: [0, 0])
    for _, sz, ft, _ in out:
        agg[ft][0] += 1
        agg[ft][1] += sz
    print('reach functions: %d' % len(out))
    print('%-17s %5s %7s  %s' % ('feature', 'fns', 'bytes', 'how a recording reaches it'))
    for ft, (n, b) in sorted(agg.items(), key=lambda x: -x[1][1]):
        print('%-17s %5d %7d  %s' % (ft, n, b, HOW.get(ft, '')))


main()
