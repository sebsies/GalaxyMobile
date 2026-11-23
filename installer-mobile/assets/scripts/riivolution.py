#!/usr/bin/env python3
"""Riivolution XML parser & on-device asset cooker for GalaxyMobile.

Supports:
  - Standard Wii Riivolution packs (riivolution/*.xml) with <wiidisc>, <id>,
    <options>/<section>/<option>/<choice>, <patch>, <folder>, <file>,
    <savegame>, and <memory> detection.
  - Loose mod folders (files/, StageData/, ObjectData/, AudioRes/, etc.)
    when no XML is present.
  - Cooking Wii big-endian / Yaz0-compressed mod assets using cook.py into
    <out_dir>/mods/<mod_id>/files/... + mod.ini + mods_config.ini.
"""
import collections
import concurrent.futures
import gc
import json
import os
import re
import shutil
import sys
import time
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))

GAME_DIRS = {
    'audiores', 'effectdata', 'layoutdata', 'localizedata', 'moviedata',
    'objectdata', 'stagedata', 'systemdata',
    'usenglish', 'usspanish', 'usfrench',
    'euenglish', 'eufrench', 'eugerman', 'euspanish', 'euitalian', 'eudutch',
    'jpjapanese', 'krkorean', 'cntaiwan',
}

FILE_COOK_EXTS = {
    '.bcsv', '.pa', '.bcam', '.banmt', '.tbl',
    '.ast', '.baa', '.cit', '.bmt', '.bme', '.brs', '.bct', '.csw',
    '.kcl', '.bas', '.bmg', '.tpl', '.bti', '.bdl', '.bmd', '.jpc',
    '.canm', '.brlyt', '.brlan', '.brfnt',
}

SKIP_LOOSE_EXTS = {
    '.txt', '.md', '.xml', '.json', '.ini', '.cfg', '.log',
    '.png', '.jpg', '.jpeg', '.bmp', '.ico', '.pdf', '.html', '.htm',
    '.py', '.bat', '.sh', '.ps1', '.exe', '.url', '.ds_store',
}


def sanitize_id(name):
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', (name or '').strip())
    cleaned = cleaned.strip('._-')
    return cleaned[:48] if cleaned else 'riivolution_mod'


def _norm_bool(val, default=False):
    if val is None:
        return default
    v = str(val).strip().lower()
    if v in ('1', 'true', 'yes', 'on'):
        return True
    if v in ('0', 'false', 'no', 'off'):
        return False
    return default


def _parse_int(val, default=0):
    if val is None or str(val).strip() == '':
        return default
    try:
        return int(str(val).strip(), 0)
    except Exception:
        return default


def _resolve_case_path(base_dir, rel_parts):
    """Resolves relative path segments under base_dir case-insensitively."""
    cur = base_dir
    for part in rel_parts:
        if not part or part == '.':
            continue
        exact = os.path.join(cur, part)
        if os.path.exists(exact):
            cur = exact
            continue
        if not os.path.isdir(cur):
            return None
        low = part.lower()
        match = None
        try:
            for entry in os.listdir(cur):
                if entry.lower() == low:
                    match = os.path.join(cur, entry)
                    break
        except Exception:
            return None
        if match is None:
            return None
        cur = match
    return cur


def _resolve_external_path(pack_root, patch_root, ext_attr):
    """Resolves a Riivolution external="..." attribute inside pack_root."""
    if not ext_attr:
        return None
    raw = ext_attr.replace('\\', '/').strip()
    is_abs_sd = raw.startswith('/')
    rel = raw.strip('/')
    rel_parts = [p for p in rel.split('/') if p and p != '..']
    patch_parts = [p for p in (patch_root or '').replace('\\', '/').strip('/').split('/') if p and p != '..']

    candidates = []
    if is_abs_sd:
        candidates.append(rel_parts)
        if patch_parts and not (len(rel_parts) >= len(patch_parts) and [x.lower() for x in rel_parts[:len(patch_parts)]] == [x.lower() for x in patch_parts]):
            candidates.append(patch_parts + rel_parts)
    else:
        if patch_parts:
            candidates.append(patch_parts + rel_parts)
        candidates.append(rel_parts)

    # Fallback: some packs sit one directory inside pack_root or put files directly under pack_root
    if rel_parts and rel_parts[0].lower() == 'files':
        candidates.append(rel_parts[1:])

    for parts in candidates:
        resolved = _resolve_case_path(pack_root, parts)
        if resolved is not None and os.path.exists(resolved):
            return resolved
    return None


def _normalize_disc_rel(disc_attr):
    """Normalizes a Riivolution disc="..." path to a relative path inside files/."""
    if not disc_attr:
        return ''
    p = disc_attr.replace('\\', '/').strip().strip('/')
    if not p:
        return ''
    parts = [s for s in p.split('/') if s and s != '.' and s != '..']
    if parts and parts[0].lower() == 'files':
        parts = parts[1:]
    return '/'.join(parts)


def _strip_xml_namespaces(elem):
    for node in elem.iter():
        if isinstance(node.tag, str) and '}' in node.tag:
            node.tag = node.tag.split('}', 1)[1]


def parse_riivolution_xml(xml_path, pack_root):
    tree = ET.parse(xml_path)
    root_el = tree.getroot()
    _strip_xml_namespaces(root_el)

    xml_stem = os.path.splitext(os.path.basename(xml_path))[0]
    wiidisc_root = root_el.attrib.get('root', '')
    mod_name = root_el.attrib.get('name', '').strip()

    game_ids = []
    sections_list = []
    options_list = []
    patches = {}

    for id_el in root_el.findall('id'):
        g = id_el.attrib.get('game', '').strip()
        if g:
            game_ids.append(g)

    options_el = root_el.find('options')
    if options_el is not None:
        for sec_idx, sec_el in enumerate(options_el.findall('section')):
            sec_name = sec_el.attrib.get('name', '').strip() or xml_stem
            if not mod_name and sec_idx == 0:
                mod_name = sec_name
            sections_list.append(sec_name)
            for opt_idx, opt_el in enumerate(sec_el.findall('option')):
                opt_id = opt_el.attrib.get('id', '').strip() or f'opt_{sec_idx}_{opt_idx}'
                opt_name = opt_el.attrib.get('name', '').strip() or opt_id
                default_val = _parse_int(opt_el.attrib.get('default'), 0)
                choices = [{'index': 0, 'name': 'Disabled', 'patches': []}]
                for ch_idx, ch_el in enumerate(opt_el.findall('choice'), 1):
                    ch_name = ch_el.attrib.get('name', '').strip() or f'Choice {ch_idx}'
                    ch_patches = []
                    for p_ref in ch_el.findall('patch'):
                        pid = p_ref.attrib.get('id', '').strip()
                        if pid:
                            ch_patches.append(pid)
                    choices.append({'index': ch_idx, 'name': ch_name, 'patches': ch_patches})
                if default_val < 0 or default_val >= len(choices):
                    default_val = 0
                options_list.append({
                    'key': f'{sec_idx}:{opt_idx}:{opt_id}',
                    'id': opt_id,
                    'name': opt_name,
                    'section': sec_name,
                    'default': default_val,
                    'choices': choices,
                })

    for p_el in root_el.findall('patch'):
        pid = p_el.attrib.get('id', '').strip()
        if not pid:
            continue
        p_root = p_el.attrib.get('root', wiidisc_root)
        files = []
        folders = []
        savegame = None
        mem_count = 0

        for child in p_el:
            tag = (child.tag or '').lower()
            if tag == 'file':
                files.append({
                    'disc': child.attrib.get('disc', ''),
                    'external': child.attrib.get('external', ''),
                    'create': _norm_bool(child.attrib.get('create'), False),
                    'resize': _norm_bool(child.attrib.get('resize'), True),
                    'offset': _parse_int(child.attrib.get('offset'), 0),
                    'length': _parse_int(child.attrib.get('length'), 0),
                })
            elif tag == 'folder':
                folders.append({
                    'disc': child.attrib.get('disc', ''),
                    'external': child.attrib.get('external', ''),
                    'recursive': _norm_bool(child.attrib.get('recursive'), True),
                    'create_attr': child.attrib.get('create', None),
                    'create': _norm_bool(child.attrib.get('create'), False),
                    'length': _parse_int(child.attrib.get('length'), 0),
                })
            elif tag == 'savegame':
                savegame = {
                    'external': child.attrib.get('external', '').strip(),
                    'clone': _norm_bool(child.attrib.get('clone'), True),
                }
            elif tag == 'memory':
                mem_count += 1

        patches[pid] = {
            'id': pid,
            'root': p_root,
            'files': files,
            'folders': folders,
            'savegame': savegame,
            'memory_count': mem_count,
        }

    if not mod_name:
        mod_name = xml_stem

    # Check Galaxy 1 compatibility (RMG / RMGE01 / RMGP01 / RMGJ01 / RMGK01)
    game_ok = True
    if game_ids:
        game_ok = any(g.upper().startswith('RMG') for g in game_ids)

    return {
        'kind': 'xml',
        'id': sanitize_id(xml_stem),
        'name': mod_name,
        'xml_path': xml_path,
        'xml_rel': os.path.relpath(xml_path, pack_root).replace('\\', '/'),
        'pack_root': pack_root,
        'game_ids': game_ids,
        'game_compatible': game_ok,
        'options': options_list,
        'patches': patches,
    }


def _find_loose_files_root(source_dir):
    """Detects an XML-less mod folder (files/ or StageData/, ObjectData/, etc.)."""
    if not os.path.isdir(source_dir):
        return None
    if os.path.isfile(os.path.join(source_dir, 'sys', 'fst.bin')):
        return None

    def has_game_dirs(d):
        try:
            names = {e.lower(): os.path.join(d, e) for e in os.listdir(d) if os.path.isdir(os.path.join(d, e))}
        except Exception:
            return False
        if 'files' in names:
            return True
        return any(k in GAME_DIRS for k in names)

    if has_game_dirs(source_dir):
        fsub = os.path.join(source_dir, 'files')
        if os.path.isdir(fsub) and has_game_dirs(fsub):
            return fsub
        return source_dir

    # Check 1-2 levels deep (e.g., extracted zip root folder)
    try:
        for entry in sorted(os.listdir(source_dir)):
            if entry.startswith('__MACOSX') or entry.startswith('.'):
                continue
            sub = os.path.join(source_dir, entry)
            if os.path.isdir(sub) and has_game_dirs(sub):
                if os.path.isfile(os.path.join(sub, 'sys', 'fst.bin')):
                    return None
                fsub = os.path.join(sub, 'files')
                if os.path.isdir(fsub) and has_game_dirs(fsub):
                    return fsub
                return sub
    except Exception:
        pass
    return None


def discover_packs(source_dir):
    """Finds all Riivolution XML packs (or a loose mod folder) in source_dir."""
    if not source_dir or not os.path.isdir(source_dir):
        return []

    xml_files = []
    base_depth = source_dir.rstrip(os.sep).count(os.sep)
    for root, dirs, files in os.walk(source_dir):
        dirs[:] = sorted(d for d in dirs if not d.startswith('__MACOSX') and not d.startswith('.'))
        depth = root.count(os.sep) - base_depth
        if depth > 4:
            dirs[:] = []
            continue
        if os.path.basename(root).lower() == 'riivolution':
            pack_root = os.path.dirname(root)
            for fn in sorted(files):
                if fn.lower().endswith('.xml') and not fn.startswith('._'):
                    xml_files.append((os.path.join(root, fn), pack_root))

    packs = []
    seen_ids = set()
    for xp, proot in xml_files:
        try:
            pack = parse_riivolution_xml(xp, proot)
            base_id = pack['id']
            idx = 2
            while pack['id'] in seen_ids:
                pack['id'] = f'{base_id}_{idx}'
                idx += 1
            seen_ids.add(pack['id'])
            packs.append(pack)
        except Exception as e:
            print(f'[Riivolution] Failed parsing {xp}: {e}')

    if packs:
        return packs

    loose_root = _find_loose_files_root(source_dir)
    if loose_root:
        folder_name = os.path.basename(os.path.normpath(source_dir))
        mod_id = sanitize_id(folder_name)
        return [{
            'kind': 'loose',
            'id': mod_id,
            'name': folder_name or 'Custom Mod',
            'xml_path': '',
            'xml_rel': '(Loose Mod Folder)',
            'pack_root': loose_root,
            'game_ids': ['RMG'],
            'game_compatible': True,
            'options': [],
            'patches': {
                'loose': {
                    'id': 'loose',
                    'root': '',
                    'files': [],
                    'folders': [{
                        'disc': '/',
                        'external': '/',
                        'recursive': True,
                        'create_attr': 'true',
                        'create': True,
                        'length': 0,
                    }],
                    'savegame': None,
                    'memory_count': 0,
                }
            },
        }]

    return []


def is_riivolution_pack(source_dir):
    return len(discover_packs(source_dir)) > 0


def inspect_source(source_dir):
    """Returns JSON-serializable inspection summary for UI/JNI callers."""
    packs = discover_packs(source_dir)
    out = []
    for p in packs:
        total_mem = sum(pt['memory_count'] for pt in p['patches'].values())
        out.append({
            'kind': p['kind'],
            'id': p['id'],
            'name': p['name'],
            'xml_rel': p['xml_rel'],
            'game_ids': p['game_ids'],
            'game_compatible': p['game_compatible'],
            'total_memory_patches': total_mem,
            'options': p['options'],
        })
    return {'packs': out}


def _load_vanilla_fst_paths(fst_path):
    """Parses big-endian Wii sys/fst.bin into a lowercase set of disc file paths."""
    if not fst_path or not os.path.isfile(fst_path):
        return None
    try:
        with open(fst_path, 'rb') as f:
            data = f.read()
        if len(data) < 12:
            return None
        import struct
        count = struct.unpack_from('>I', data, 8)[0]
        if count * 12 > len(data):
            return None
        str_base = count * 12
        strings = data[str_base:]

        def get_name(off):
            end = strings.find(b'\0', off)
            if end < 0:
                end = len(strings)
            return strings[off:end].decode('latin1', errors='replace')

        paths = set()
        stack = []
        i = 1
        while i < count:
            while stack and stack[-1][1] <= i:
                stack.pop()
            rec_off = i * 12
            val0, parent_or_off, length_or_next = struct.unpack_from('>III', data, rec_off)
            is_dir = (val0 >> 24) != 0
            name_off = val0 & 0x00FFFFFF
            name = get_name(name_off)
            cur_parts = [s[0] for s in stack] + [name]
            full = '/'.join(cur_parts).lower()
            if is_dir:
                stack.append((name, length_or_next))
            else:
                paths.add(full)
            i += 1
        return paths
    except Exception as e:
        print(f'[Riivolution] Warning: could not inspect fst.bin ({e})')
        return None


def resolve_mod_plan(pack, option_choices=None, vanilla_fst_paths=None):
    """Resolves active patches, mapped disc files, sys files, savegame, and warnings."""
    option_choices = option_choices or {}
    active_patch_ids = []

    if pack['kind'] == 'xml' and pack['options']:
        for opt in pack['options']:
            sel = None
            if opt['key'] in option_choices:
                sel = _parse_int(option_choices[opt['key']], opt['default'])
            elif opt['id'] in option_choices:
                sel = _parse_int(option_choices[opt['id']], opt['default'])
            else:
                sel = opt['default']
            if 0 < sel < len(opt['choices']):
                active_patch_ids.extend(opt['choices'][sel]['patches'])
    else:
        active_patch_ids = list(pack['patches'].keys())

    # Deduplicate patch ids preserving order
    seen_pids = set()
    ordered_pids = []
    for pid in active_patch_ids:
        if pid in pack['patches'] and pid not in seen_pids:
            seen_pids.add(pid)
            ordered_pids.append(pid)

    mapped_disc_files = {}   # disc_rel -> (src_path, is_new)
    mapped_sys_files = {}    # sys_name -> src_path
    savegame_cfg = None
    ppc_memory_patches = 0
    offset_patches_skipped = 0

    def record_disc_file(disc_rel, src_path, create_allowed, explicit_create_false):
        nonlocal mapped_disc_files
        norm_key = disc_rel.lower()
        # Filter obvious non-game documentation/metadata in root folder mappings
        ext = os.path.splitext(disc_rel)[1].lower()
        top = disc_rel.split('/')[0].lower() if '/' in disc_rel else ''
        if top in ('riivolution', '__macosx'):
            return
        if ext in SKIP_LOOSE_EXTS and (vanilla_fst_paths is None or norm_key not in vanilla_fst_paths):
            return

        is_in_vanilla = (vanilla_fst_paths is None) or (norm_key in vanilla_fst_paths)
        if not is_in_vanilla and explicit_create_false and not create_allowed:
            return
        mapped_disc_files[disc_rel] = (src_path, not is_in_vanilla)

    pack_root = pack['pack_root']
    for pid in ordered_pids:
        patch = pack['patches'][pid]
        p_root = patch['root']
        ppc_memory_patches += patch['memory_count']
        if patch['savegame'] and patch['savegame'].get('external'):
            savegame_cfg = patch['savegame']

        for f_rule in patch['files']:
            if f_rule['offset'] > 0:
                offset_patches_skipped += 1
                continue
            disc_raw = f_rule['disc'].replace('\\', '/').strip().strip('/')
            low_raw = disc_raw.lower()
            if low_raw == 'main.dol' or low_raw.endswith('/main.dol'):
                ppc_memory_patches += 1
                continue

            src = _resolve_external_path(pack_root, p_root, f_rule['external'])
            if not src or not os.path.isfile(src):
                continue

            if low_raw in ('sys/storyevent.bcsv', 'storyevent.bcsv'):
                mapped_sys_files['StoryEvent.bcsv'] = src
                continue
            if low_raw in ('sys/galaxyid.bcsv', 'galaxyid.bcsv'):
                mapped_sys_files['GalaxyID.bcsv'] = src
                continue
            if low_raw in ('sys/errormessagearchive.arc', 'errormessagearchive.arc'):
                mapped_sys_files['ErrorMessageArchive.arc'] = src
                continue

            disc_rel = _normalize_disc_rel(f_rule['disc'])
            if not disc_rel:
                disc_rel = os.path.basename(src)
            create_false = not f_rule['create']
            record_disc_file(disc_rel, src, f_rule['create'], False if f_rule['create'] else False)

        for d_rule in patch['folders']:
            src_dir = _resolve_external_path(pack_root, p_root, d_rule['external'])
            if not src_dir or not os.path.isdir(src_dir):
                continue
            disc_base = _normalize_disc_rel(d_rule['disc'])
            explicit_false = (d_rule['create_attr'] is not None) and (not d_rule['create'])
            if d_rule['recursive']:
                for cur_root, dirs, files in os.walk(src_dir):
                    dirs[:] = sorted(d for d in dirs if not d.startswith('__MACOSX') and not d.startswith('.'))
                    rel_dir = os.path.relpath(cur_root, src_dir).replace('\\', '/')
                    if rel_dir == '.':
                        rel_dir = ''
                    for fn in sorted(files):
                        if fn.startswith('._'):
                            continue
                        full_src = os.path.join(cur_root, fn)
                        parts = [x for x in (disc_base, rel_dir, fn) if x]
                        disc_rel = '/'.join(parts)
                        record_disc_file(disc_rel, full_src, d_rule['create'], explicit_false)
            else:
                try:
                    for fn in sorted(os.listdir(src_dir)):
                        if fn.startswith('._'):
                            continue
                        full_src = os.path.join(src_dir, fn)
                        if os.path.isfile(full_src):
                            parts = [x for x in (disc_base, fn) if x]
                            disc_rel = '/'.join(parts)
                            record_disc_file(disc_rel, full_src, d_rule['create'], explicit_false)
                except Exception:
                    pass

    return {
        'active_patches': ordered_pids,
        'disc_files': mapped_disc_files,
        'sys_files': mapped_sys_files,
        'savegame': savegame_cfg,
        'ppc_memory_patches': ppc_memory_patches,
        'offset_patches_skipped': offset_patches_skipped,
    }


def _classify_cook_mode(src_path, disc_rel):
    ext = os.path.splitext(disc_rel)[1].lower()
    if ext in ('.arc', '.szs'):
        try:
            with open(src_path, 'rb') as f:
                magic = f.read(4)
            if magic == b'CRAR':
                return 'copy'
        except Exception:
            pass
        return 'archive'
    if ext == '.thp':
        return 'thp'
    if ext in FILE_COOK_EXTS or ext == '' or (len(ext) > 1 and ext[1:].isdigit()):
        return 'file'
    # Check magic for extensionless / unusual files
    try:
        with open(src_path, 'rb') as f:
            magic = f.read(4)
        if magic in (b'RARC', b'Yaz0'):
            return 'archive'
        if magic in (b'J3D2', b'J3D1', b'RLYT', b'RLAN', b'RFNT', b'MESG', b'ANDO', b'JPAC', b'AA_<', b'STRM'):
            return 'file'
    except Exception:
        pass
    return 'copy'


def _read_mods_config(cfg_path):
    entries = []
    if not os.path.isfile(cfg_path):
        return entries
    try:
        with open(cfg_path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                s = line.strip()
                if not s or s.startswith('#'):
                    continue
                if '|' in s:
                    state, mid = s.split('|', 1)
                    mid = mid.strip()
                    if mid:
                        entries.append((state.strip().lower() == 'enabled', mid))
    except Exception:
        pass
    return entries


def _write_mods_config(cfg_path, entries):
    os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
    with open(cfg_path, 'w', encoding='utf-8') as f:
        f.write('# GalaxyMobile Riivolution Mods Config\n')
        f.write('# Order from top to bottom = highest to lowest priority\n')
        for enabled, mid in entries:
            flag = 'enabled' if enabled else 'disabled'
            f.write(f'{flag}|{mid}\n')


def register_mod_in_config(out_data_dir, mod_id, enable=True):
    cfg_path = os.path.join(out_data_dir, 'mods', 'mods_config.ini')
    entries = _read_mods_config(cfg_path)
    found = False
    new_entries = []
    for en, mid in entries:
        if mid == mod_id:
            new_entries.append((enable, mid))
            found = True
        else:
            new_entries.append((en, mid))
    if not found:
        new_entries.insert(0, (enable, mod_id))
    _write_mods_config(cfg_path, new_entries)


def cook_pack(pack, out_data_dir, option_choices=None, progress=None):
    """Cooks one Riivolution pack into <out_data_dir>/mods/<mod_id>/."""
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import cook

    fst_bin = os.path.join(out_data_dir, 'sys', 'fst.bin')
    vanilla_paths = _load_vanilla_fst_paths(fst_bin)
    plan = resolve_mod_plan(pack, option_choices=option_choices, vanilla_fst_paths=vanilla_paths)

    mod_id = pack['id']
    mod_dir = os.path.join(out_data_dir, 'mods', mod_id)
    files_out = os.path.join(mod_dir, 'files')
    sys_out = os.path.join(mod_dir, 'sys')

    # Clean previous files from this mod so switching options doesn't leave stale overlays
    if os.path.isdir(files_out):
        shutil.rmtree(files_out, ignore_errors=True)
    if os.path.isdir(sys_out):
        shutil.rmtree(sys_out, ignore_errors=True)
    os.makedirs(files_out, exist_ok=True)

    jobs = []
    created_count = 0
    for disc_rel, (src_path, is_new) in sorted(plan['disc_files'].items()):
        dst = os.path.join(files_out, disc_rel.replace('/', os.sep))
        mode = _classify_cook_mode(src_path, disc_rel)
        jobs.append((src_path, dst, mode))
        if is_new:
            created_count += 1

    for sys_name, src_path in sorted(plan['sys_files'].items()):
        dst = os.path.join(sys_out, sys_name)
        mode = 'archive' if sys_name.lower().endswith('.arc') else 'file'
        jobs.append((src_path, dst, mode))

    if not jobs:
        raise RuntimeError(
            f"No mod asset files resolved for '{pack['name']}'. "
            f"(Active patches: {plan['active_patches']}, PPC memory patches: {plan['ppc_memory_patches']})"
        )

    print(f"[Riivolution] Cooking mod '{pack['name']}' (id={mod_id}): {len(jobs)} file(s), {created_count} new")
    if plan['ppc_memory_patches'] > 0:
        print(
            f"[Riivolution] Note: {plan['ppc_memory_patches']} Wii PowerPC <memory>/DOL patch(es) detected. "
            "GalaxyMobile runs natively on ARM64 C++, so asset replacements apply while PPC code patches are skipped."
        )
    if plan['offset_patches_skipped'] > 0:
        print(f"[Riivolution] Note: {plan['offset_patches_skipped']} byte-offset patch(es) skipped.")

    if progress:
        progress(0, len(jobs))

    t0 = time.time()
    total = collections.Counter()
    notes = collections.Counter()
    errors = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
        for done, (stats, n, err) in enumerate(ex.map(cook.process, jobs), 1):
            total.update(stats)
            notes.update(n)
            if err:
                errors.append(err)
            if progress and (done % 4 == 0 or done == len(jobs)):
                progress(done, len(jobs))
            if done % 16 == 0:
                gc.collect()

    gc.collect()
    elapsed = time.time() - t0
    print(f"[Riivolution] Cooked '{pack['name']}' ({len(jobs)} files) in {elapsed:.1f}s")
    for msg, v in notes.most_common(15):
        print(f"  note ({v}x): {msg}")
    for e in errors[:10]:
        print(f"  [Warning] {e}")

    save_id = ''
    clone_save = '1'
    if plan['savegame'] and plan['savegame'].get('external'):
        save_id = sanitize_id(plan['savegame']['external']) or mod_id
        clone_save = '1' if plan['savegame'].get('clone', True) else '0'

    mod_ini_path = os.path.join(mod_dir, 'mod.ini')
    with open(mod_ini_path, 'w', encoding='utf-8') as f:
        f.write(f"id={mod_id}\n")
        f.write(f"name={pack['name'].replace(chr(10), ' ').strip()}\n")
        f.write(f"xml={pack['xml_rel']}\n")
        f.write(f"save_id={save_id}\n")
        f.write(f"clone_save={clone_save}\n")
        f.write(f"ppc_patches={plan['ppc_memory_patches']}\n")
        f.write(f"file_count={len(jobs)}\n")
        f.write(f"created_count={created_count}\n")

    register_mod_in_config(out_data_dir, mod_id, enable=True)

    return {
        'id': mod_id,
        'name': pack['name'],
        'files_cooked': len(jobs),
        'created_files': created_count,
        'ppc_patches': plan['ppc_memory_patches'],
        'save_id': save_id,
        'errors': len(errors),
    }


def install_from_dir(source_dir, out_data_dir, pack_id=None, options_json=None, progress=None):
    """High-level entry point called by py_bridge / cook.convert."""
    packs = discover_packs(source_dir)
    if not packs:
        raise FileNotFoundError(f"No Riivolution XML or mod asset folders found in {source_dir}")

    opt_map = {}
    if options_json:
        try:
            opt_map = json.loads(options_json) if isinstance(options_json, str) else dict(options_json)
        except Exception:
            opt_map = {}

    if pack_id:
        selected = [p for p in packs if p['id'] == pack_id]
        if not selected:
            raise ValueError(f"Riivolution pack '{pack_id}' not found in {source_dir}")
        packs = selected
    elif len(packs) > 1:
        # Prefer SMG1-compatible packs when installing multiple XMLs at once
        rmg_packs = [p for p in packs if p['game_compatible']]
        if rmg_packs:
            packs = rmg_packs

    results = []
    for idx, pack in enumerate(packs):
        if not pack['game_compatible']:
            print(f"[Riivolution] Skipping '{pack['name']}' (targeted game IDs {pack['game_ids']}, expected RMG*).")
            continue
        per_pack_opts = opt_map.get(pack['id'], opt_map) if isinstance(opt_map, dict) else {}
        res = cook_pack(pack, out_data_dir, option_choices=per_pack_opts, progress=progress)
        results.append(res)

    if not results:
        raise RuntimeError("No Super Mario Galaxy (RMG*) compatible Riivolution packs found.")
    return results
