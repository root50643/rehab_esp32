"""Check the ESP32 over the same LAN, using its STA IPv4 address.

Never sends GPIO/TCP controls. --save-unchanged saves identical settings,
omitting the password, and increases the NVS revision once. No history files.
"""
import argparse
import ipaddress
import json
import time
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def ipv4(text):
    try:
        address = ipaddress.IPv4Address(text)
        if address.is_unspecified or address.is_multicast or address.is_loopback:
            raise ValueError()
        return str(address)
    except ValueError as exc:
        raise argparse.ArgumentTypeError('Use the ESP32 STA IPv4 address on the same LAN') from exc


def redacted(value):
    if isinstance(value, dict):
        assert all('password' not in key.lower() or key == 'has_password' for key in value), 'Password field exposed'
        for item in value.values(): redacted(item)
    elif isinstance(value, list):
        for item in value: redacted(item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host', type=ipv4)
    parser.add_argument('--save-unchanged', action='store_true')
    parser.add_argument('--expect-revision', type=int)
    args = parser.parse_args()
    base = 'http://' + args.host
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(path, body=None, origin=None, expected=200, host=None):
        headers = {} if body is None else {'Content-Type': 'application/json', 'Origin': origin or base}
        if origin is not None: headers['Origin'] = origin
        if host is not None: headers['Host'] = host
        req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        try: response = opener.open(req, timeout=4)
        except urllib.error.HTTPError as exc: response = exc
        with response:
            assert response.code == expected, f'HTTP check failed: expected {expected}, received {response.code}'
            assert not response.headers.get('Location'), 'LAN response unexpectedly redirects'
            data = response.read(2 * 1024 * 1024 + 1)
            assert len(data) <= 2 * 1024 * 1024, 'Response exceeded limit'
            return data

    def config():
        result = json.loads(request('/api/config')); redacted(result); return result

    before = config()
    print(f'INFO: initial NVS revision={before["revision"]}', flush=True)
    if args.expect_revision is not None:
        assert before['revision'] == args.expect_revision, 'Persisted NVS revision does not match expectation'
    assert before['pending'] is False, 'Complete pending settings before this check'
    status = json.loads(request('/api/status')); redacted(status)
    assert {'ap', 'wifi', 'tcp', 'ble', 'machine', 'rfid', 'health'} <= status.keys(), 'Incomplete status API'
    assert status['wifi']['state'] == 'connected' and status['wifi']['ip'] == args.host, 'Use the connected STA address'
    for path in ('/', '/app.js', '/style.css'):
        assert len(request(path)) > 100, 'Static asset is missing or empty'
    request('/__lan_smoke_missing__', expected=404)
    request('/generate_204', expected=404)
    saved = before['settings']
    body = {key: saved[key] for key in ('wifi_ssid', 'socket_server', 'heart_rate_address')}
    body['revision'] = before['revision']
    request('/api/config', body, origin='http://cross-origin.invalid', expected=403)
    assert config() == before, 'Cross-origin rejection changed configuration'
    for payload in (None, body):
        request('/api/config', payload, origin='http://forged.invalid', host='forged.invalid', expected=403)
    assert config() == before, 'Forged Host/Origin rejection changed configuration'
    assert json.loads(request('/api/config', origin=base, host=args.host + ':80')) == before, 'Explicit default HTTP port failed'
    wrong = dict(body, revision=1 if before['revision'] != 1 else 2)
    request('/api/config', wrong, origin=base, host=args.host + ':80', expected=409)
    assert config() == before, 'Revision rejection changed configuration'
    print('PASS: same-LAN assets/API, password redaction, 404, cross-origin and revision rejection', flush=True)
    if args.save_unchanged:
        machine = status['machine']
        assert not machine['tracking'] and machine['pulse'] == 'idle' and machine['queued_controls'] == 0, 'Controller must be idle for save verification'
        result = json.loads(request('/api/config', body)); redacted(result)
        assert result['revision'] == before['revision'] + 1, 'Unexpected saved revision'
        deadline = time.monotonic() + 15
        while True:
            after = config()
            assert after['settings'] == saved and after['revision'] == result['revision'], 'Settings changed during verification'
            if not after['pending']: break
            assert time.monotonic() < deadline, 'Identical settings remain pending'
            time.sleep(.25)
        assert after['active'] == before['active'], 'Active settings changed'
        print(f'PASS: identical settings applied; NVS revision={after["revision"]}', flush=True)


if __name__ == '__main__':
    try: main()
    except (OSError, ValueError, AssertionError, KeyError) as exc:
        raise SystemExit(f'FAIL: {exc}') from None
