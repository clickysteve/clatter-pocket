// A simulated FM-1 at the Web MIDI level, for testing the installer with no device. It stands
// where the browser's MIDIAccess does, and M-VAVE's real sdk/WebUpdate.js talks to it.
//
// It speaks the protocol as WebUpdate.js expects it: identity (17), capability (49), arm (50), the
// start message, then block requests (48), through the running firmware's stage and the loader's,
// with the re-enumeration between them. It was written from that client code, not from a device,
// so it shows that the installer behaves, and not that a real FM-1 will. It runs no firmware.
//
// Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only

const START = [240, 34, 36, 53, 127, 247];
const LOADER = 'ota-FM-1_015';
const STAGE_ONE = 0xe0000000, STAGE_TWO = 0xf0000000;
const hexBytes = hex => hex.match(/../g).map(s => parseInt(s, 16));
const later = (fn, ms = 0) => setTimeout(fn, ms);

class Port extends EventTarget {
  constructor(name, device, kind) { super(); Object.assign(this, { name, manufacturer: 'M-VAVE', state: 'connected', device, kind, connection: 'closed' }); }
  async open() { if (this.device.jammed) throw new Error('port busy'); this.connection = 'open'; }
  async close() { this.connection = 'closed'; }
  send(bytes) { this.device.receive(Uint8Array.from(bytes)); }
}
const message = data => Object.assign(new Event('midimessage'), { data });

export class SimulatedFM1 {
  /**
   * options: identity, build, family, state (capability state), imageBytes, name, mode, newBuild,
   * and fault: what goes wrong (see where `this.fault` is read).
   */
  constructor(FM1Web, access, options = {}) {
    Object.assign(this, {
      FM1Web, access, identity: 'FM-1_8000014', build: '11'.repeat(32), family: 7, state: 0, imageBytes: 0,
      name: 'FM-1', fault: null, gap: 2, mode: 'app', ...options,
    });
    this.received = new Uint8Array(this.imageBytes);
    this.next = 0;
    this.log = [];
    this.writes = 0;
    this.plug();
  }

  // ---------------------------------------------------------------- ports
  plug() {
    this.input = new Port(this.name, this, 'in');
    this.output = new Port(this.name, this, 'out');
    this.access.inputs.set(`${this.name}-in-${this.log.length}`, this.input);
    this.access.outputs.set(`${this.name}-out-${this.log.length}`, this.output);
    this.ports = [this.input, this.output];
  }

  unplug() {
    for (const port of this.ports ?? []) port.state = 'disconnected';
    for (const map of [this.access.inputs, this.access.outputs])
      for (const [key, port] of map) if (port.device === this) map.delete(key);
    this.ports = [];
  }

  /** The device goes away and comes back as `mode` after `ms`. */
  reenumerate(mode, ms = 15) {
    this.unplug();
    later(() => { this.mode = mode; this.plug(); }, ms);
  }

  // ---------------------------------------------------------------- the protocol
  send(command, payload) {
    if (this.input?.state !== 'connected') return;
    const frame = this.FM1Web.frame(command, payload);
    later(() => this.input?.dispatchEvent(message(frame)));
  }

  reportedIdentity() {
    if (this.mode === 'loader') return LOADER;
    if (this.fault === 'identity-changes' && this.log.includes('capability')) return 'FM-1_8000099';
    return this.identity;
  }

  capability() {
    const cap = new Uint8Array(47);
    cap.set([...'FMC1'].map(c => c.charCodeAt(0)));
    cap[4] = 1; cap[5] = this.family; cap[6] = this.state; cap[7] = 1;
    cap[8] = 0; cap[9] = 2;                                   // block size 512
    cap.set([0, 0, 16, 0], 10);                               // flash 1 MiB
    cap.set(hexBytes(this.fault === 'wrong-build' && this.mode === 'rebooted' ? 'dd'.repeat(32) : this.build), 14);
    return [...cap];
  }

  receive(bytes) {
    if (this.fault === 'silent') return;
    if (bytes.length === 6 && START.every((b, i) => b === bytes[i])) { this.log.push('start'); this.begin(); return; }
    const packet = this.FM1Web.parse(bytes);
    if (!packet) return;
    if (packet.command === 17) {
      this.log.push('identity');
      const reply = new Array(27).fill(0);
      [...this.reportedIdentity()].forEach((c, i) => { reply[i] = c.charCodeAt(0); });
      this.send(17, reply);
    } else if (packet.command === 49) {
      this.log.push('capability');
      if (this.fault === 'no-capability') return;
      this.send(49, this.fault === 'bad-capability' ? [1, 2, 3] : this.capability());
    } else if (packet.command === 50) {
      this.log.push('arm');
      this.send(50, [this.fault === 'refuse' ? 1 : 0]);
    } else if (packet.command === 48) {
      this.block(packet.data);
    }
  }

  begin() {
    this.stage = this.mode === 'loader' ? 2 : 1;
    this.next = 0;
    this.request();
  }

  request() {
    const finish = this.stage === 1 ? STAGE_ONE : STAGE_TWO;
    // the running firmware fetches a little to start the loader; the loader fetches it all
    const end = this.stage === 1 ? Math.min(1024, this.imageBytes) : this.imageBytes;
    let address = this.next, bytes = Math.min(512, end - this.next);
    if (this.fault === 'stall' && this.stage === 2 && this.next > 2048) return;
    if (this.fault === 'unplug' && this.stage === 2 && this.next >= 4096) { this.unplug(); return; }
    if (this.fault === 'out-of-range' && this.stage === 2) { address = this.imageBytes + 512; bytes = 512; }
    else if (this.next >= end) {
      address = this.fault === 'wrong-stage' && this.stage === 2 ? STAGE_ONE : finish;
      bytes = 8;
    }
    const p = [this.stage, address & 255, (address >>> 8) & 255, (address >>> 16) & 255, address >>> 24, bytes & 255, bytes >>> 8, 0];
    later(() => this.send(48, p), this.gap);
  }

  block(data) {
    const address = data[1] + data[2] * 256 + data[3] * 65536 + data[4] * 16777216;
    const body = data.subarray(8);
    if (address === STAGE_ONE || address === STAGE_TWO) {
      this.log.push(address === STAGE_ONE ? 'stage-one-done' : 'stage-two-done');
      if (address === STAGE_ONE) this.reenumerate('loader');
      else if (this.fault === 'never-returns') this.unplug();
      else this.reboot();
      return;
    }
    if (this.stage === 2) {
      this.received.set(body, address);
      this.writes += body.length;
    }
    this.next = address + body.length;
    this.request();
  }

  /** The loader has what it was sent: the device restarts, running that. */
  reboot() {
    this.written = this.received.slice();
    this.unplug();
    later(() => { this.mode = 'rebooted'; this.build = this.newBuild ?? this.build; this.state = 0; this.plug(); }, 15);
  }

  /** The cable goes back in after a `fault: 'unplug'`: the loader is still running. */
  replug() {
    this.fault = null;
    this.mode = 'loader';
    this.plug();
  }
}

/** A stand-in for navigator.requestMIDIAccess's result. */
export const simulatedAccess = () => ({ inputs: new Map(), outputs: new Map(), sysexEnabled: true });
