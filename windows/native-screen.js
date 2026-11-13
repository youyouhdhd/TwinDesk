// TwinDesk — Copyright (C) 2026 inukaasith. GPL-3.0, see LICENSE.
// WGC -> native shared GPU textures -> the existing WebRTC encoder. No CPU pixel conversion.
'use strict';
const { sharedTexture } = require('electron');
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const EXE = path.join(__dirname, '..', 'windows-helper', 'bin', `twindesk-capture-${process.arch}.exe`);

class NativeScreen {
  constructor(frame, session, onError) {
    this.frame = frame;
    this.session = session;
    this.onError = onError;
    this.proc = null;
    this.stopped = false;
  }

  static available() { return !!sharedTexture && fs.existsSync(EXE); }

  start(rect, fps) {
    return new Promise((resolve, reject) => {
      let ready = false, buffer = '';
      const fail = error => {
        if (this.stopped) return;
        clearTimeout(timer);
        this.cancel = null;
        this.stop();
        if (ready) this.onError(error);
        else reject(error);
      };
      const timer = setTimeout(() => fail(new Error('Native screen capture timed out')), 5000);
      const proc = this.proc = spawn(EXE, [process.pid, rect.x, rect.y, rect.width, rect.height, fps].map(String),
        { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
      const release = index => {
        if (!this.stopped && proc.stdin.writable) proc.stdin.write(index + '\n');
      };
      proc.stdin.on('error', () => {});
      proc.stderr.on('data', data => console.warn('[native-screen]', String(data).trim()));
      proc.on('error', fail);
      proc.on('exit', code => fail(new Error('Native screen capture exited: ' + code)));
      proc.stdout.setEncoding('utf8');
      proc.stdout.on('data', data => {
        buffer += data;
        let end;
        while ((end = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, end); buffer = buffer.slice(end + 1);
          if (this.stopped) continue;
          try {
            const m = JSON.parse(line);
            if (m.error) { fail(new Error(m.error)); continue; }
            if (m.ready) {
              ready = true; clearTimeout(timer);
              this.delivered = Date.now();
              // A dropped hand-off may keep its slot busy; with every slot stuck no frames come at all.
              this.watchdog = setInterval(() => {
                if (Date.now() - this.delivered > 3000) fail(new Error('No frame delivered for 3 s'));
              }, 500);
              resolve({ width: m.width, height: m.height });
              continue;
            }
            const handle = Buffer.alloc(8);
            handle.writeBigUInt64LE(BigInt(m.handle));
            const texture = sharedTexture.importSharedTexture({
              textureInfo: { handle: { ntHandle: handle }, pixelFormat: 'bgra',
                codedSize: { width: m.width, height: m.height }, timestamp: m.timestamp,
                colorSpace: { primaries: 'bt709', transfer: 'iec61966-2-1', matrix: 'rgb', range: 'full' } },
              allReferencesReleased: () => release(m.frame),
            });
            // sendSharedTexture gives up after 1 s. A slow first hand-off (B: ~1 s after start, every
            // time) drops that frame only; a run of misses, or no frame delivered for 3 s, ends WGC.
            const sent = Date.now();
            sharedTexture.sendSharedTexture({ frame: this.frame, importedSharedTexture: texture }, this.session)
              .then(() => {
                if (this.misses) console.log('[native-screen] frames delivered again after ' + this.misses + ' missed');
                this.misses = 0; this.delivered = Date.now();
                if (!this.firstDelivered) { this.firstDelivered = true; console.log('[native-screen] first frame delivered in ' + (Date.now() - sent) + ' ms'); }
              })
              .catch(error => {
                if (this.stopped) return;
                this.misses = (this.misses || 0) + 1;
                console.warn('[native-screen] frame ' + m.frame + ' not delivered (' + this.misses + ' in a row): ' + error.message);
                if (this.misses >= 5) fail(new Error('Frames not delivered (' + this.misses + ' in a row): ' + error.message));
              })
              .finally(() => texture.release());
          } catch (error) { fail(error); }
        }
      });
      this.cancel = () => { clearTimeout(timer); if (!ready) reject(new Error('Native screen capture cancelled')); };
    });
  }

  stop() {
    if (this.stopped) return;
    this.stopped = true;
    clearInterval(this.watchdog);
    if (this.cancel) this.cancel();
    const proc = this.proc;
    if (!proc) return;
    proc.stdin.end();
    const kill = setTimeout(() => { if (proc.exitCode == null) proc.kill(); }, 2000);
    kill.unref();
    proc.once('exit', () => clearTimeout(kill));
  }
}
module.exports = NativeScreen;
