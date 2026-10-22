"""Real Chromium + Gateway integration; binary and three full-resolution H264 AUs required."""
import os
from pathlib import Path
import select
import struct
import subprocess
import sys

from playwright.sync_api import sync_playwright, expect


ROOT = Path(__file__).resolve().parents[2]
SOURCES = [('rgb.main', 2566, 1520), ('rgb.sub', 1920, 1080), ('thermal.main', 640, 512)]
assert len(sys.argv) == 5, 'usage: browser_test.py binary main.h264 sub.h264 thermal.h264'
assert (ROOT / 'www/dist/index.html').exists(), 'first run npm ci && npm run build in www'
env = dict(os.environ, UAV_GATEWAY_WEB_ROOT=str(ROOT / 'www/dist'), UAV_WEB_TEST_SHORT_TTL='1')
server = subprocess.Popen([sys.argv[1], '--serve-three', *sys.argv[2:]], env=env, cwd='/tmp',
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def card(page, source):
    return page.locator(f'article[data-source="{source}"]')


def decoded(page, source, width, height):
    page.wait_for_function('''({source, width, height}) => {
        const video = document.querySelector(`article[data-source="${source}"] video`);
        return video.videoWidth === width && video.videoHeight === height &&
            video.getVideoPlaybackQuality().totalVideoFrames > 3;
    }''', arg=dict(source=source, width=width, height=height), timeout=45000)
    expect(card(page, source).locator('.stream-badge')).to_have_attribute('data-state', 'playing')


def frames(page, source):
    return card(page, source).locator('video').evaluate('(v) => v.getVideoPlaybackQuality().totalVideoFrames')


def login(page, invalid=False):
    page.get_by_test_id('login-open').click()
    if invalid:
        page.locator('#credential').fill('wrong-token')
        page.locator('dialog[open] button[type=submit]').click()
        expect(page.get_by_role('alert')).to_contain_text('接入凭据无效')
    page.locator('#credential').fill('live-viewer')
    page.locator('dialog[open] button[type=submit]').click()
    expect(page.locator('.user-menu')).to_contain_text('观看用户')
    expect(page.locator('dialog.auth-dialog')).not_to_be_visible()
    assert page.locator('#credential').input_value() == ''
    assert page.evaluate('localStorage.length === 0 && sessionStorage.length === 0')


try:
    if not select.select([server.stdout], [], [], 20)[0]:
        raise RuntimeError('test server startup timed out')
    line = server.stdout.readline().strip()
    assert line.startswith('PORT '), line
    url = 'http://127.0.0.1:' + line.split()[1] + '/webrtc'
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, args=['--no-sandbox'])
        errors, offers, renewals, refreshes = [], [], [], []
        page = browser.new_page(viewport=dict(width=1440, height=1080), device_scale_factor=1)
        page.on('pageerror', lambda error: errors.append(str(error)))

        def request(req):
            if req.method == 'POST':
                if '/api/v1/webrtc/' in req.url:
                    offers.append(req.url)
                if req.url.endswith('/renew'):
                    renewals.append(req.url)
                if req.url.endswith('/auth/refresh'):
                    refreshes.append(req.url)

        page.on('request', request)
        try:
            page.goto(url)
            expect(page.get_by_role('heading', name='多视角实时监看')).to_be_visible()
            page.screenshot(path='/tmp/uav-console-desktop.png', full_page=True)
            login(page, invalid=True)
            page.get_by_test_id('start-all').click()
            for source, width, height in SOURCES:
                decoded(page, source, width, height)
            page.screenshot(path='/tmp/uav-console-live.png', full_page=True)
            # Renewal rotates the media token; access expiry must refresh without losing video.
            baseline = {source: frames(page, source) for source, _, _ in SOURCES}
            page.wait_for_timeout(10000)
            for source, _, _ in SOURCES:
                assert frames(page, source) > baseline[source] + 3, f'{source} stopped after renewal'
            assert len(renewals) >= 3 and refreshes, (renewals, refreshes)
            expect(card(page, 'rgb.main').locator('.bitrate-spark')).to_be_visible()
            page.get_by_role('button', name='等分视图', exact=True).click()
            page.get_by_role('button', name='设备状态', exact=True).click()
            expect(page.locator('.device-card')).to_have_count(2)
            page.get_by_role('button', name='连接记录', exact=True).click()
            expect(page.get_by_text('实时画面已接入', exact=True)).to_have_count(3)
            page.get_by_role('button', name='实时监看', exact=False).first.click()
            assert len(offers) == 3, 'layout/navigation must not create additional peers'
            with page.expect_download() as downloaded:
                page.get_by_role('button', name='保存可见光主通道截图', exact=True).click()
            path = '/tmp/uav-console-snapshot.png'
            downloaded.value.save_as(path)
            assert struct.unpack('>II', Path(path).read_bytes()[16:24]) == (2566, 1520)
            page.get_by_role('button', name='全屏查看可见光主通道', exact=True).click()
            page.wait_for_function('document.fullscreenElement !== null')
            page.evaluate('document.exitFullscreen()')

            second = browser.new_page(viewport=dict(width=1440, height=1000))
            second.on('pageerror', lambda error: errors.append(str(error)))
            second.goto(url)
            login(second)
            second.get_by_role('button', name='连接可见光主通道', exact=True).click()
            expect(card(second, 'rgb.main')).to_contain_text('该视频源正在被其他连接使用')
            other_frames = frames(page, 'rgb.sub')
            page.get_by_role('button', name='停止可见光主通道', exact=True).click()
            expect(card(page, 'rgb.main').locator('.stream-badge')).to_have_attribute('data-state', 'idle')
            card(second, 'rgb.main').get_by_role('button', name='重新连接', exact=True).click()
            decoded(second, 'rgb.main', 2566, 1520)
            assert frames(page, 'rgb.sub') > other_frames
            page.get_by_test_id('stop-all').click()
            for source, _, _ in SOURCES:
                expect(card(page, source).locator('.stream-badge')).to_have_attribute('data-state', 'idle')
            page.get_by_role('button', name='退出登录', exact=True).click()
            expect(page.get_by_test_id('login-open')).to_be_visible()
            expect(card(second, 'rgb.main').locator('.stream-badge')).to_have_attribute('data-state', 'playing')

            page.set_viewport_size(dict(width=390, height=844))
            page.screenshot(path='/tmp/uav-console-mobile.png', full_page=True)
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'), 'mobile page overflows'
            page.get_by_role('button', name='连接帮助', exact=True).click()
            expect(page.get_by_role('heading', name='连接与观看')).to_be_visible()
            page.keyboard.press('Escape')
            expect(page.locator('dialog.help-panel')).not_to_be_visible()
            # Page departure releases the source and revokes that tab's login session.
            second.goto('about:blank')
            page.set_viewport_size(dict(width=1440, height=1000))
            login(page)
            page.get_by_role('button', name='连接可见光主通道', exact=True).click()
            decoded(page, 'rgb.main', 2566, 1520)
            page.get_by_test_id('stop-all').click()
            expect(card(page, 'rgb.main').locator('.stream-badge')).to_have_attribute('data-state', 'idle')
            assert not errors, errors
            print('PASS: console login, three full-resolution videos, live statistics, token renewal/refresh,')
            print('layout/navigation, snapshot/fullscreen, per-source slot reuse, logout/page exit and mobile layout')
            print('Screenshots: /tmp/uav-console-desktop.png /tmp/uav-console-live.png /tmp/uav-console-mobile.png')
        except Exception:
            print('Console:', page.locator('body').inner_text())
            print('Page errors:', errors)
            page.screenshot(path='/tmp/uav-console-failure.png', full_page=True)
            raise
        finally:
            browser.close()
finally:
    server.terminate()
    server.wait(timeout=15)
    assert server.returncode == 0, server.returncode
