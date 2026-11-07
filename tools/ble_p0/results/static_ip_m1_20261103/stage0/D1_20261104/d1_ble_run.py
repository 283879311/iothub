#!/usr/bin/env python3
import asyncio, json, secrets, sys
from pathlib import Path
sys.path.insert(0, str(Path('tools/ble_p0').resolve()))
from p0_acceptance import Session, sanitize, scan_filtered, choose_target, clear_cached_iot_targets
from p0_acceptance import CMD_TX
from bleak import BleakClient

ADDR = 'B0:A7:32:28:C8:E6'
OUT = Path(sys.argv[1])

def data_of(obj):
    if not isinstance(obj, dict):
        return {}
    d = obj.get('data')
    return d if isinstance(d, dict) else {}

async def main():
    result = {'target': ADDR, 'tests': {}, 'blocked': {}}
    clear_cached_iot_targets()
    rows = await scan_filtered(10)
    target = choose_target(rows, ADDR)
    if target is None or 'C8E4' not in (target.get('name') or ''):
        raise SystemExit('C8E4 target not found / wrong target')
    result['name'] = target.get('name')
    result['scan_hits'] = [{'address': r.get('address'), 'name': r.get('name')} for r in rows]
    cli = BleakClient(target['address'], timeout=25)
    await cli.connect()
    sess = Session(cli, address=target['address'], username='admin', password='123456')
    await cli.start_notify(CMD_TX, sess.on_notify)
    await asyncio.sleep(0.2)
    await sess.login()

    async def v1(name, cmd, args=None, timeout=20):
        r = await sess.v1(cmd, args or {}, timeout=timeout, id_prefix='d1-'+name)
        result['tests'][name] = sanitize(r.obj)
        return r

    # D1-01 / D1-02
    await v1('device_info', 'device.info', {})
    await v1('device_status', 'device.status', {})
    await v1('config_get', 'config.get', {})
    await v1('network_status', 'network.status', {})
    st = data_of(result['tests']['network_status'].get('data', {})) if isinstance(result['tests']['network_status'], dict) else {}
    result['current_ssid'] = st.get('sta_ssid')

    # D1-03: raw v0 .async must be rejected by policy
    try:
        r = await sess.v0('network.tryconnect.async', {'ssid': result.get('current_ssid') or 'CMCC-502', 'password': 'x'}, req_id=101, timeout=15)
        result['tests']['v0_async_tryconnect'] = sanitize(r.obj)
    except Exception as e:
        result['tests']['v0_async_tryconnect'] = {'error': repr(e)}

    # D1-06: create an active transaction with a deliberately wrong password.
    ssid = result.get('current_ssid') or 'CMCC-502'
    try:
        r = await v1('tryconnect_active', 'network.tryconnect', {
            'ssid': ssid, 'password': 'd1-wrong-' + secrets.token_hex(6),
            'idempotency_key': 'd1-gate-' + secrets.token_hex(4)
        }, timeout=20)
        op_id = data_of(r.obj).get('operation_id')
    except Exception as e:
        result['tests']['tryconnect_active'] = {'error': repr(e)}
        op_id = None
    result['operation_id'] = op_id
    result['operation_initial'] = data_of(r.obj)

    async def op_state():
        if not op_id:
            return None
        rr = await sess.v1('operation.get', {'operation_id': op_id}, timeout=10, id_prefix='d1-opget')
        result.setdefault('operation_polls', []).append(sanitize(rr.obj))
        return data_of(rr.obj).get('state')

    state = await op_state()
    result['operation_state_before_gate'] = state

    # D1-08: auth.admin.set during active transaction must be busy.
    try:
        await v1('auth_admin_active', 'auth.admin.set', {
            'old_password': '123456', 'new_password': '123456'
        }, timeout=15)
    except Exception as e:
        result['tests']['auth_admin_active'] = {'error': repr(e)}
    # D1-09: nonexistent profile delete during active transaction must be busy.
    try:
        await v1('profile_delete_active', 'network.profile.delete', {'ssid': '__gate_probe'}, timeout=15)
    except Exception as e:
        result['tests']['profile_delete_active'] = {'error': repr(e)}
    # Supplemental BLE config.set gate (MQTT version is blocked by missing broker access).
    try:
        r = await sess.v0('config.set', {'__gate_probe': 1}, req_id=102, timeout=15)
        result['tests']['ble_config_set_active'] = sanitize(r.obj)
    except Exception as e:
        result['tests']['ble_config_set_active'] = {'error': repr(e)}

    # Cancel transaction
    if op_id:
        try:
            await v1('cancel', 'network.cancel', {'operation_id': op_id}, timeout=15)
        except Exception as e:
            result['tests']['cancel'] = {'error': repr(e)}
    result['operation_state_after_cancel'] = await op_state()

    # D1-10: gate released; unknown field ignored, no config change.
    try:
        r = await sess.v0('config.set', {'__gate_probe': 1}, req_id=103, timeout=15)
        result['tests']['ble_config_set_after'] = sanitize(r.obj)
    except Exception as e:
        result['tests']['ble_config_set_after'] = {'error': repr(e)}

    # D1-11: conditional last_profile.
    r = await v1('profiles', 'network.profile.list', {}, timeout=20)
    profiles = []
    d = data_of(r.obj)
    if isinstance(d.get('profiles'), list):
        profiles = d['profiles']
    result['profiles'] = profiles
    if len(profiles) == 1 and isinstance(profiles[0], dict) and profiles[0].get('ssid'):
        target_ssid = profiles[0]['ssid']
        r = await v1('last_profile_delete', 'network.profile.delete', {'ssid': target_ssid}, timeout=15)
        result['last_profile_ssid'] = target_ssid
    else:
        result['last_profile_skipped'] = f'profile_count={len(profiles)}'

    # D1-04/05 MQTT items are not executable without broker credentials.
    result['blocked']['mqtt_v1'] = 'missing MQTT broker credential in environment'
    OUT.write_text(json.dumps(result, ensure_ascii=False, indent=2))
    await cli.disconnect()

asyncio.run(main())
