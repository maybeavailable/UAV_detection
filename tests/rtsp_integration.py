"""Local RTSP correctness regressions; no board or network security checks."""
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time


class Client:
    def __init__(self, port, slow=False):
        self.port = port
        self.s = socket.socket()
        if slow:
            self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        self.s.settimeout(3)
        self.s.connect(('127.0.0.1', port))
        self.buf = b''
        self.seq = 0
        self.session = None

    def fill(self, size):
        while len(self.buf) < size:
            data = self.s.recv(65536)
            if not data:
                raise EOFError('RTSP socket closed')
            self.buf += data

    def item(self):
        self.fill(4)
        if self.buf[:1] == b'$':
            size = int.from_bytes(self.buf[2:4], 'big')
            self.fill(size + 4)
            item = self.buf[1], self.buf[4:size+4]
            self.buf = self.buf[size+4:]
            return item
        while b'\r\n\r\n' not in self.buf:
            self.fill(len(self.buf)+1)
        end = self.buf.index(b'\r\n\r\n')+4
        headers = self.buf[:end].decode()
        match = re.search(r'(?im)^Content-Length: (\d+)', headers)
        size = int(match[1]) if match else 0
        self.fill(end+size)
        body = self.buf[end:end+size]
        self.buf = self.buf[end+size:]
        return headers, body

    def request(self, method, path, extra='', status=200):
        self.seq += 1
        request = f'{method} rtsp://127.0.0.1:{self.port}{path} RTSP/1.0\r\nCSeq: {self.seq}\r\n'
        if self.session:
            request += f'Session: {self.session}\r\n'
        self.s.sendall((request+extra+'\r\n').encode())
        while True:
            headers, body = self.item()
            if isinstance(headers, int):
                continue
            assert headers.startswith(f'RTSP/1.0 {status} '), headers
            assert f'CSeq: {self.seq}\r\n' in headers
            match = re.search(r'(?im)^Session: ([^;\r]+)', headers)
            if match:
                self.session = match[1]
            return headers, body

    def setup(self, path, transport='RTP/AVP/TCP;unicast;interleaved=0-1', status=200):
        return self.request('SETUP', path+'/track1', 'Transport: '+transport+'\r\n', status)

    def packet(self):
        while True:
            channel, data = self.item()
            if channel == 0:
                return data

    def play(self, path, ssrc):
        headers, _ = self.request('PLAY', path)
        seq = int(re.search(r'(?im)^RTP-Info: .*;seq=(\d+)', headers)[1])
        assert 'rtptime=0' not in headers
        p = self.packet()
        actual_seq, _, actual_ssrc = struct.unpack('!HII', p[2:12])
        assert actual_seq == seq, (seq, actual_seq)
        assert actual_ssrc == ssrc, (ssrc, actual_ssrc)
        return p

    def close(self):
        self.s.close()


def rr(ssrc):
    return struct.pack('!BBHIIIIIII', 0x81, 201, 7, 0x11223344, ssrc, 0, 99, 90, 0, 0)


def generate(ffmpeg, path, frames, params='', size='64x64'):
    subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                    f'testsrc2=size={size}:rate=25', '-frames:v', str(frames), '-c:v', 'libx264',
                    '-threads', '1', '-x264-params', 'slices=4:keyint=5:min-keyint=5:scenecut=0:bframes=0'+params,
                    '-f', 'h264', '-y', str(path)], check=True, timeout=15)


def main():
    unit, driver, ffmpeg = map(str, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='rtsp-regression-') as tmp:
        work = Path(tmp)
        multi, media = work/'multi.h264', work/'media.h264'
        generate(ffmpeg, multi, 2)
        generate(ffmpeg, media, 50)
        bframes, aud, changed = work/'bframes.h264', work/'aud.h264', work/'changed.h264'
        generate(ffmpeg, bframes, 8, ':bframes=2')
        generate(ffmpeg, aud, 8, ':aud=1')
        generate(ffmpeg, changed, 4)
        part = work/'part.h264'
        generate(ffmpeg, part, 4, size='96x64')
        changed.write_bytes(changed.read_bytes()+part.read_bytes())
        variants = [bframes, aud, changed]
        subprocess.run([unit, str(multi), *map(str, variants)], cwd=work, check=True, timeout=15)
        for variant in variants:
            # 重组后的参数集与切片必须仍可由独立解码器解码，且图像数不能改变。
            result = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-xerror',
                '-i', str(variant)+'.grouped.h264', '-f', 'null', '-'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=8)
            assert result.returncode == 0, result.stderr.decode()
        print('PASS: access unit boundaries with B frames, AUD and changing SPS/resolution')
        output = open(work/'server.stdout', 'w+')
        proc = subprocess.Popen([driver, str(media)], cwd=work, stdout=output, stderr=subprocess.STDOUT)
        clients = []
        udp_sockets = []
        try:
            until = time.monotonic()+6
            while time.monotonic() < until:
                text = (work/'server.stdout').read_text()
                if text.startswith('ready '):
                    break
                assert proc.poll() is None, text
                time.sleep(.02)
            else:
                raise AssertionError('test server startup timeout')
            _, port, a_ssrc, b_ssrc = text.splitlines()[0].split()
            port, a_ssrc, b_ssrc = int(port), int(a_ssrc), int(b_ssrc)

            c = Client(port); clients.append(c)
            c.request('OPTIONS', '/live/a')
            h, sdp = c.request('DESCRIBE', '/live/a')
            assert f':{port}/live/a/' in h
            assert b'packetization-mode=1' in sdp
            c.setup('/live/b')  # DESCRIBE A 后 SETUP B 必须绑定 B
            c.play('/live/b', b_ssrc)
            c.request('DESCRIBE', '/live/a')  # 活跃会话的归属不能被 DESCRIBE 改写
            c.play('/live/b', b_ssrc)
            c.request('PLAY', '/live/a', status=455)
            c.setup('/live/a', status=455)
            c.request('PAUSE', '/live/b')
            # 消费已到达数据后，暂停状态不再发送新媒体。
            c.s.settimeout(.15)
            try:
                c.item()
                raise AssertionError('media after PAUSE response')
            except socket.timeout:
                pass
            c.s.settimeout(3)
            c.setup('/live/a', status=455)
            c.setup('/live/b')
            c.play('/live/b', b_ssrc)
            c.s.sendall(b'$\x01'+struct.pack('!H', len(rr(b_ssrc)))+rr(b_ssrc))
            c.request('GET_PARAMETER', '/live/b')
            c.request('TEARDOWN', '/live/b')
            c.request('PLAY', '/live/b', status=454)
            c.session = None
            c.setup('/live/a'); c.play('/live/a', a_ssrc)
            c.request('TEARDOWN', '/live/a'); c.close()

            u = Client(port); clients.append(u)
            for _ in range(2):
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sock.bind(('127.0.0.1', 0)); sock.settimeout(3); udp_sockets.append(sock)
            rtp, rtcp = udp_sockets
            h, _ = u.setup('/live/a', f'RTP/AVP;unicast;client_port={rtp.getsockname()[1]}-{rtcp.getsockname()[1]}')
            server_rtcp = int(re.search(r'server_port=\d+-(\d+)', h)[1])
            rtcp.sendto(rr(a_ssrc), ('127.0.0.1', server_rtcp))
            h, _ = u.request('PLAY', '/live/a')
            seq = int(re.search(r';seq=(\d+)', h)[1])
            p, _ = rtp.recvfrom(4096)
            assert struct.unpack('!H', p[2:4])[0] == seq
            assert struct.unpack('!I', p[8:12])[0] == a_ssrc
            u.request('PAUSE', '/live/a'); u.request('TEARDOWN', '/live/a'); u.close()
            until = time.monotonic()+2
            while time.monotonic() < until:
                log = (work/'server.stdout').read_text()
                if f'feedback {a_ssrc} 99' in log and f'feedback {b_ssrc} 99' in log:
                    break
                time.sleep(.02)
            else:
                raise AssertionError('TCP/UDP RR feedback missing: '+log)
            print('PASS: RTSP control, source binding, first-packet sequence, TCP/UDP and RTCP feedback')

            slow = Client(port, slow=True); clients.append(slow)
            slow.setup('/live/a'); slow.request('PLAY', '/live/a')
            fast = Client(port); clients.append(fast)
            fast.setup('/live/b'); fast.play('/live/b', b_ssrc)
            deadline = time.monotonic()+3
            count = 0
            while time.monotonic() < deadline:
                fast.packet(); count += 1
            assert count > 100
            fast.request('GET_PARAMETER', '/live/b')
            fast.request('TEARDOWN', '/live/b'); fast.close()
            log = (work/'logs/uav_detection.log').read_text()
            assert 'slow client exceeded write queue limit' in log, log
            slow.close()
            print('PASS: slow client closed at queue limit while fast client keeps streaming')

            for transport in ('tcp', 'udp'):
                result = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'warning',
                    '-rtsp_transport', transport, '-i', f'rtsp://127.0.0.1:{port}/file',
                    '-frames:v', '10', '-f', 'null', '-'], stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, timeout=12)
                assert result.returncode == 0, result.stderr.decode()
                assert 'Output file is empty' not in result.stderr.decode()
                print(f'PASS: FFmpeg {transport.upper()} decodes 10 multi-slice frames')
        finally:
            for c in clients:
                c.close()
            for sock in udp_sockets:
                sock.close()
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
            output.close()
            server_output = (work/'server.stdout').read_text()
            assert proc.returncode == 0, server_output
            assert 'AddressSanitizer' not in server_output and 'runtime error:' not in server_output, server_output


if __name__ == '__main__':
    main()
