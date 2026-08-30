/**
 * Pigeon sim — third person over photorealistic London.
 *
 * The world is Google's Photorealistic 3D Tiles, streamed live. That is a real
 * dependency on the network and on a billed API key, taken deliberately: no
 * open dataset carries street-level imagery of London, and at pigeon altitude
 * imagery is most of what you see.
 *
 * Flight is flappy-bird, not flight-sim. One key, one beat: each press buys a
 * lump of airspeed and a shove of lift, and the only thing holding you up in
 * between is the speed you already have. Gravity does the rest.
 */
import {
  Scene, PerspectiveCamera, WebGLRenderer, Group, Vector3, Vector2, Quaternion,
  Matrix4, Euler, DirectionalLight, AmbientLight, MathUtils, Fog, Color,
  Mesh, MeshStandardMaterial, SphereGeometry, BoxGeometry, ConeGeometry, Raycaster,
} from 'three';
import { TilesRenderer, WGS84_ELLIPSOID } from '3d-tiles-renderer';
import { GoogleCloudAuthPlugin, GLTFExtensionsPlugin, TileCompressionPlugin } from '3d-tiles-renderer/plugins';
import { DRACOLoader } from 'three/examples/jsm/loaders/DRACOLoader.js';

const DEG = MathUtils.DEG2RAD;

// Over the Thames by Tower Bridge: dense, recognisable, and with the river to
// one side so there is always something to orient against.
const HOME = { lat: 51.5045, lon: -0.0754, height: 180 };

// Columba livia, as measured by people who measure birds: 300-350 g, 65 cm of
// wingspan, cruising near 18 m/s, and beating its wings five to eight times a
// second. That last number is the one that decides how this feels. A pigeon is
// not a glider that occasionally flaps — it is a flapper that occasionally
// glides, badly, at about four metres forward per metre down.
//
// So one press is not one big heave. Holding the key beats continuously at a
// real frequency and each beat is small, which is why staying up is work.
const TUNE = {
  gravity: 9.81,          // the actual one; the weight comes from the polar below

  // Sink rate as a function of airspeed: induced drag falling off as 1/v, form
  // drag climbing as v^3. Fitted to a pigeon — least sink about 2.5 m/s at
  // 12 m/s, best glide a shade over five to one near 16 m/s. Flying slower than
  // best glide costs you more, not less, which is the bit that surprises people.
  polarA: 22.5,
  polarB: 3.617e-4,

  // 26 m/s is 94 km/h. A pigeon pressed hard does about that; the old 34 was
  // aeroplane speed and made every height feel the same.
  speedStall: 5.5, speedBestGlide: 13, speedMax: 26,

  // A beat at cruise buys about a metre per second of climb and a third of a
  // metre of speed; at 6.5 Hz that is a climb of roughly three metres a second,
  // which is what a pigeon actually manages.
  // One press is one wingbeat, because on the board it is one button press and
  // a button cannot be held at six hertz. Each beat injects power that bleeds
  // away over about half a second, so what the bird flies on is your cadence:
  //
  //   beatImpulse * pressesPerSecond * powerDecay = climb rate it can pay for
  //
  // At 5.8 and 0.5 s one press a second buys 2.9 m/s, which is exactly the sink
  // at cruise — so one a second holds height, two climbs, three climbs hard.
  // The cadence Kevin asked for falls out of these two numbers rather than
  // being clamped on top of them.
  beatImpulse: 5.8, powerDecay: 0.5, powerCap: 13, strokeTime: 0.26,
  // Off the deck it is a different gait: faster, deeper, and mostly forward,
  // because at a standstill there is no airspeed to make lift out of.
  // Off the deck a beat is worth more: deeper, and against no airspeed at all,
  // which is how a pigeon clatters off a pavement.
  burstImpulse: 11, burstBelow: 9,
  // However much power there is, the nose only comes up so far. Without this the
  // balance answers a standstill by pointing straight up and hanging there;
  // capped, the surplus has nowhere to go but forward, which is a takeoff.
  trimMax: 0.42,

  // Speed you can carry depends on how high you are. A pigeon crossing a square
  // at head height is not doing 60 km/h; it does that on the way somewhere, up
  // where there is nothing to hit. Low and slow, high and fast — which is what
  // makes a landing a landing and a takeoff worth the effort, without either
  // needing a special case anywhere.
  groundSpeed: 3.5,       // ceiling at the deck, m/s — about walking pace
  speedHeight: 55,        // metres up at which the full speed range is back
  flareTime: 0.9,         // how quickly speed bleeds towards that ceiling
  flareHeight: 10,        // below this the nose is held up rather than stalled
  perchSpeed: 3.2,        // slow enough, low enough, and the bird is standing

  // A beat a second is sustainable; three is not, and that is the whole budget.
  staminaMax: 100, staminaPerBeat: 2.6, staminaRegen: 3.1,

  turnRate: 1.7,          // rad/s the heading chases the pointer
  boardTurn: 2.2,         // rad/s of yaw at full tilt on the board
  bankPerTurn: 1.0,
  // A third of a metre of bird wants the camera close. This is most of why the
  // old build felt slow: speed is only ever read against something you know the
  // size of, and that something is the bird.
  camBack: 1.75, camUp: 0.42, camLag: 0.07,
};

/* ---- scene ---------------------------------------------------------------- */

const scene = new Scene();
scene.background = new Color(0x9fc4e0);
scene.fog = new Fog(0x9fc4e0, 900, 4200);

const camera = new PerspectiveCamera(62, 2, 1, 40000);
const renderer = new WebGLRenderer({ antialias: true });
renderer.setPixelRatio(Math.min(2, devicePixelRatio || 1));
document.body.append(renderer.domElement);

scene.add(new AmbientLight(0xbdd4e8, 1.5));
const sun = new DirectionalLight(0xfff2e0, 2.1);
sun.position.set(0.5, 1, 0.3);
scene.add(sun);

/* ---- the bird ------------------------------------------------------------- */
// A stand-in, deliberately: enough of a silhouette to read attitude and heading
// against the city, cheap enough to replace the moment a real model turns up.

function buildBird() {
  const g = new Group();
  const body = new Mesh(new SphereGeometry(0.085, 16, 12),
                        new MeshStandardMaterial({ color: 0x5b6472, roughness: 0.85 }));
  body.scale.set(1.9, 0.95, 0.8);
  g.add(body);

  const head = new Mesh(new SphereGeometry(0.048, 14, 10),
                        new MeshStandardMaterial({ color: 0x49586b, roughness: 0.8 }));
  head.position.set(0.15, 0.042, 0);
  g.add(head);

  const beak = new Mesh(new ConeGeometry(0.014, 0.05, 8),
                        new MeshStandardMaterial({ color: 0xd9a066, roughness: 0.6 }));
  beak.position.set(0.20, 0.032, 0);
  beak.rotation.z = -Math.PI/2;
  g.add(beak);

  const tail = new Mesh(new BoxGeometry(0.12, 0.009, 0.09),
                        new MeshStandardMaterial({ color: 0x3e4654, roughness: 0.9 }));
  tail.position.set(-0.19, 0.009, 0);
  g.add(tail);

  // Wings pivot at the shoulder, so a beat is a rotation of the pivot rather
  // than anything animated into the geometry.
  const wings = [];
  for (const side of [1, -1]) {
    const pivot = new Group();
    const wing = new Mesh(new BoxGeometry(0.15, 0.008, 0.29),
                          new MeshStandardMaterial({ color: 0x66707e, roughness: 0.85 }));
    wing.position.set(-0.018, 0, side * 0.17);
    pivot.add(wing);
    g.add(pivot);
    wings.push({ pivot, side });
  }
  return { group: g, wings };
}

const bird = buildBird();
scene.add(bird.group);

/* ---- droppings ------------------------------------------------------------ */
// BOOT drops one. It keeps the bird's momentum at release and only then falls
// behind, which is what makes leading the shot feel like anything at all.

const droppings = [];
const dropGeometry = new SphereGeometry(0.035, 8, 6);
const dropMaterial = new MeshStandardMaterial({ color: 0xf2f0e6, roughness: 0.9 });

function drop() {
  if (!state.started) return;
  const mesh = new Mesh(dropGeometry, dropMaterial);
  mesh.position.copy(state.pos);
  scene.add(mesh);
  droppings.push({
    mesh,
    vel: new Vector3().copy(state.vel).multiplyScalar(0.9),
    life: 12,
  });
}

function updateDroppings(dt) {
  for (let i = droppings.length - 1; i >= 0; i--) {
    const d = droppings[i];
    d.vel.y -= 9.81 * dt;
    d.mesh.position.addScaledVector(d.vel, dt);
    d.life -= dt;
    const floor = groundY === null ? -Infinity : groundY;
    if (d.life <= 0 || d.mesh.position.y < floor) {
      scene.remove(d.mesh);
      droppings.splice(i, 1);
    }
  }
}

/* ---- state ---------------------------------------------------------------- */

const state = {
  pos: new Vector3(0, HOME.height, 0),
  vel: new Vector3(16, 0, 0),
  heading: 0,            // radians, 0 = +x
  pitch: 0,              // radians, positive nose up
  bank: 0,
  speed: 0,              // airspeed along the flight path
  gamma: 0,              // flight path angle, radians, positive climbing
  power: 0, stroke: 1, beats: 0, beatRate: 0,
  stamina: TUNE.staminaMax,
  started: false, perched: false, placed: false,
};

const camPos = new Vector3().copy(state.pos);

/* ---- input ---------------------------------------------------------------- */
// Pointer steers, space flaps. Pointer lock so the mouse can keep turning past
// the edge of the screen, with a plain-mousemove fallback before it is granted.

const look = { yaw: 0, pitch: 0 };
// A third of what it was. The pointer asks the bird to bank round; a bird that
// snapped to the mouse would read as weightless however good the model under it.
const SENS = 0.00085;

renderer.domElement.addEventListener('click', () => renderer.domElement.requestPointerLock?.());
addEventListener('mousemove', event => {
  if (document.pointerLockElement !== renderer.domElement) return;
  look.yaw -= event.movementX * SENS;
  look.pitch = MathUtils.clamp(look.pitch - event.movementY * SENS * 0.7, -0.85, 0.7);
});

// One press, one beat. The board has two buttons and no way to hold a cadence,
// so the cadence is the player's: about once a second to hold height, two or
// three times a second to climb.
function beat() {
  if (state.stamina < TUNE.staminaPerBeat) return;
  const slow = Math.max(0, Math.min(1, (TUNE.burstBelow - state.speed) / TUNE.burstBelow));
  state.power = Math.min(TUNE.powerCap,
                         state.power + MathUtils.lerp(TUNE.beatImpulse, TUNE.burstImpulse, slow));
  state.stamina -= TUNE.staminaPerBeat;
  state.stroke = 0;
  state.beats++;
  state.perched = false;
  state.started = true;
  document.getElementById('hint')?.classList.add('gone');
}

addEventListener('keydown', event => {
  // Auto-repeat is the operating system's cadence, not the player's, and it
  // would let a held key out-flap anything a thumb could manage.
  if (event.code === 'Space') { event.preventDefault(); if (!event.repeat) beat(); }
  if (event.code === 'KeyR') respawn();
});
addEventListener('mousedown', event => { if (event.button === 0) beat(); });

function respawn() {
  state.pos.set(0, HOME.height, 0);
  state.speed = 16; state.gamma = 0;
  state.vel.set(16, 0, 0);
  state.stamina = TUNE.staminaMax;
  state.placed = false;
  look.yaw = 0; look.pitch = 0;
  state.heading = 0; state.pitch = 0; state.started = false;
}

function headingVector() {
  return new Vector3(Math.cos(state.heading), 0, Math.sin(state.heading));
}

/* ---- the board ------------------------------------------------------------ */
// The ESP32 controller, over the host's Server-Sent Events stream. Tilt steers,
// PWR beats the wings, BOOT drops one.
//
// Attitude is measured against a stored reference pose, not against a fixed
// axis frame. Deriving which device axis points up from how the panel is
// rotated was wrong twice in this project — the second time inverted, which is
// what made roll snap between +180 and -180. Held in the flying position the
// IMU reads ax = -0.88, so screen-up is -ax; that is only the default, and
// pressing BOOT-and-holding, or C on the keyboard, replaces it with wherever
// the board actually is. Working from a reference also kills the wrap: both
// angles come out of asin about that pose, so level reads zero and nothing
// crosses a discontinuity short of 90 degrees, which no pigeon reaches.

// The measured frame, from the dogfight controller's calibration.json rather
// than derived from how the panel is rotated. Deriving it was wrong twice —
// the second time inverted, which is what made roll snap between +180 and -180
// — and an earlier version of this file then used a third wrong value taken
// from game.html, reading roll and pitch off the wrong axes entirely.
//
//   {"up": [0,0,-1], "right": [1,0,0], "forward": [0,1,0]}
//
// So screen-up is -az, screen-right is +ax, and forward is +ay. These are only
// the defaults: pressing C stores whatever pose the board is actually in.
const LEVEL_DEFAULT = [0, 0, -1];
const SCREEN_RIGHT = [1, 0, 0];
const TILT_SPAN = 38;                // degrees of tilt for full deflection

// Tilt the board back and the bird climbs, like pulling a stick. The measured
// frame reports that as a negative angle, so it is flipped here rather than by
// quietly reordering the cross product — one constant, one character to change
// if it turns out to read backwards in the hand.
const BOARD_PITCH_SIGN = -1;
const BOARD_ROLL_SIGN = 1;

const dot3 = (a, b) => a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
const unit3 = a => { const n = Math.hypot(a[0],a[1],a[2]) || 1; return [a[0]/n, a[1]/n, a[2]/n]; };
const cross3 = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];

function frameFrom(level) {
  const up = unit3(level);
  const k = dot3(SCREEN_RIGHT, up);
  const right = unit3([SCREEN_RIGHT[0]-up[0]*k, SCREEN_RIGHT[1]-up[1]*k, SCREEN_RIGHT[2]-up[2]*k]);
  // cross(right, up), not cross(up, right): the other order gives -forward,
  // which reads as pitch inverted.
  return { up, right, forward: cross3(right, up) };
}
const asDegrees = v => Math.asin(MathUtils.clamp(v, -1, 1)) * 180 / Math.PI;

const board = {
  live: false,
  frame: frameFrom(LEVEL_DEFAULT),
  roll: 0, pitch: 0,          // normalised -1..1, smoothed
  level: [0, 0],              // last button levels, for edge detection
  last: null,
};

function readTilt(sample) {
  const gravity = unit3([sample[2], sample[3], sample[4]]);
  return {
    roll: BOARD_ROLL_SIGN * asDegrees(dot3(gravity, board.frame.right)) / TILT_SPAN,
    pitch: BOARD_PITCH_SIGN * asDegrees(dot3(gravity, board.frame.forward)) / TILT_SPAN,
  };
}

function recentreBoard() {
  if (board.last) board.frame = frameFrom([board.last[2], board.last[3], board.last[4]]);
}

function connectBoard() {
  const stream = new EventSource('/stream');
  stream.onmessage = event => {
    const players = JSON.parse(event.data).players;
    if (!players || !players.length) return;
    const samples = players[0].samples || [];
    for (const sample of samples) {
      // Rising edges only: the firmware sends levels, and one press has to mean
      // one beat however long a thumb stays down.
      const left = sample[8] | 0, right = sample[9] | 0;
      if (right && !board.level[1]) beat();
      if (left && !board.level[0]) drop();
      board.level = [left, right];
      board.last = sample;
    }
    const newest = samples.at(-1);
    if (!newest) return;
    if (!board.live) { board.live = true; document.getElementById('hint')?.classList.add('gone'); }
    const tilt = readTilt(newest);
    // The link arrives in bursts every 20-80 ms, so this eases towards the
    // newest reading and the flight loop runs on its own clock regardless.
    board.roll += (MathUtils.clamp(tilt.roll, -1, 1) - board.roll) * 0.35;
    board.pitch += (MathUtils.clamp(tilt.pitch, -1, 1) - board.pitch) * 0.35;
  };
  stream.onerror = () => { board.live = false; };
}
connectBoard();

addEventListener('keydown', event => { if (event.code === 'KeyC') recentreBoard(); });

/* ---- the world ------------------------------------------------------------ */

const tiles = new TilesRenderer();
// The tileset URL only exists once the key has been fetched and the auth plugin
// has run. The render loop starts immediately, so without this the first frame
// asks the renderer to load a tileset called "null", it 404s, and it stays
// failed for the rest of the session — a blank sky with a working simulation
// underneath it.
let tilesReady = false;
const draco = new DRACOLoader();
// Google's own hosted decoder. Everything else here is vendored into the repo;
// this one file cannot be, because the host only ever serves proto-*.html and a
// wasm decoder needs a real directory to sit in.
draco.setDecoderPath('https://www.gstatic.com/draco/versioned/decoders/1.5.7/');

async function startTiles() {
  const key = (await (await fetch('/googlekey')).text()).trim();
  tiles.registerPlugin(new GoogleCloudAuthPlugin({ apiToken: key, autoRefreshToken: true }));
  tiles.registerPlugin(new GLTFExtensionsPlugin({ dracoLoader: draco }));
  tiles.registerPlugin(new TileCompressionPlugin());
  tiles.setCamera(camera);
  tiles.setResolutionFromRenderer(camera, renderer);

  // The tiles arrive in earth-centred coordinates. Putting the inverse of the
  // frame at HOME on the group lands that spot at the origin with Y up, so the
  // whole simulation can be plain local metres and never think about the globe.
  const frame = new Matrix4();
  WGS84_ELLIPSOID.getObjectFrame(HOME.lat * DEG, HOME.lon * DEG, 0, 0, 0, 0, frame);
  frame.invert();
  tiles.group.matrix.copy(frame);
  tiles.group.matrixAutoUpdate = false;
  tiles.group.matrixWorldNeedsUpdate = true;
  scene.add(tiles.group);

  // Clear the overlay when geometry actually arrives, not when the manifest
  // does: the tileset JSON lands long before there is any city to look at.
  tilesReady = true;

  const waitForCity = setInterval(() => {
    if (tiles.stats.loaded > 8) {
      document.getElementById('loading')?.remove();
      clearInterval(waitForCity);
    }
  }, 250);
}
startTiles().catch(problem => {
  const el = document.getElementById('loading');
  if (el) el.textContent = 'Could not reach Google 3D Tiles: ' + problem.message;
  console.error(problem);
});

/* ---- ground ---------------------------------------------------------------- */
// Real collision, against the tile meshes themselves. The alternative — a flat
// plane at a guessed height — is what put the bird underneath London: the local
// origin sits on the WGS84 ellipsoid, and the actual ground here is some forty
// metres above that, before any building is counted.
//
// Only loaded tiles can be hit, but the ones under the camera are exactly the
// ones the level-of-detail logic guarantees are present, so the surface is
// always there when it matters. A miss returns null and is treated as "no
// opinion" rather than as ground at zero.

const downRay = new Raycaster();
downRay.far = 3000;
const DOWN = new Vector3(0, -1, 0);
const probe = new Vector3();

let groundY = null, groundAge = 1;

function sampleGround(pos) {
  probe.set(pos.x, pos.y + 600, pos.z);
  downRay.set(probe, DOWN);
  const hits = [];
  // tiles.raycast rejects whole subtrees on their bounding volumes first, which
  // is what keeps this affordable against a couple of hundred streamed meshes.
  tiles.raycast(downRay, hits);
  if (!hits.length) return null;
  let best = -Infinity;
  for (const hit of hits) if (hit.point.y > best) best = hit.point.y;
  return best;
}

/* ---- simulation ----------------------------------------------------------- */

/** Sink rate in still air at a given airspeed — the glide polar. Below about
 *  five metres a second the induced term runs away, which is true but useless,
 *  so it is held at the stall. */
function sinkRate(v) {
  const speed = Math.max(TUNE.speedStall, v);
  return TUNE.polarA / speed + TUNE.polarB * speed * speed * speed;
}

function update(dt) {
  // --- the beat -----------------------------------------------------------
  // Off the deck a pigeon uses a different gait: faster, deeper, mostly
  // forward, because at a standstill there is no airspeed to make lift from.
  // Power bleeds away between beats, so holding height is a rhythm rather than
  // a state: miss a beat and you feel it before you see it.
  state.power *= Math.exp(-dt / TUNE.powerDecay);
  state.stroke = Math.min(1, state.stroke + dt / TUNE.strokeTime);
  state.stamina = Math.min(TUNE.staminaMax, state.stamina + TUNE.staminaRegen * dt);
  // Beats per second, smoothed, purely so the HUD can show the cadence back.
  state.beatRate += ((state.power / (TUNE.beatImpulse * TUNE.powerDecay)) - state.beatRate)
                    * (1 - Math.exp(-dt / 0.4));

  // --- energy -------------------------------------------------------------
  // One balance does the whole job. The polar takes g*sink out per second, the
  // wings put g*power back, and whatever is left over is spent climbing or
  // banked as speed depending on how the bird is trimmed.
  const sink = sinkRate(state.speed);
  const vRef = Math.max(TUNE.speedStall, state.speed);
  const beatVy = state.power;

  // The angle at which effort exactly balances: coasting that is the glide
  // slope, working it is the climb the muscles can actually hold. Trimming to
  // it means neither state quietly bleeds speed, which is what made the last
  // version stall itself every time you held the key.
  const authority = Math.min(1, state.speed / TUNE.speedBestGlide);
  // Near the ground the bird is flaring, not stalling, so the steepest descent
  // it will trim to is gentle. Without this the low-speed ceiling would read as
  // a stall and drop the nose into the pavement on every approach.
  const flare = 1 - MathUtils.clamp(
    (groundY === null ? 999 : state.pos.y - groundY) / TUNE.flareHeight, 0, 1);
  const trim = MathUtils.clamp(
    Math.asin(MathUtils.clamp((beatVy - sink) / vRef, -0.98, 0.98)),
    MathUtils.lerp(-1.2, -0.10, flare), TUNE.trimMax);

  // Pulling up costs airspeed, so the authority to do it fades out as the
  // airspeed goes. Pushing down never fades — the way out of a stall is always
  // available, which is what stops a held key flying the bird into the ground.
  const margin = MathUtils.clamp(
    (state.speed - TUNE.speedStall) / (TUNE.speedBestGlide - TUNE.speedStall), 0, 1);
  // The board's tilt outranks the pointer whenever a board is connected, so a
  // controller and a mouse never fight over the same bird.
  const pitchInput = board.live ? board.pitch * 0.85 : look.pitch;
  const asked = pitchInput * 0.85 * (pitchInput > 0 ? margin * margin : 1);
  const wanted = MathUtils.clamp(trim + asked, -1.35, TUNE.trimMax + 0.35);
  state.gamma += (wanted - state.gamma) * (1 - Math.exp(-dt / 0.16));

  // Speed is what is left when the climb is paid for. Point the nose below trim
  // and the difference comes back as airspeed.
  state.speed += TUNE.gravity * ((beatVy - sink) / vRef - Math.sin(state.gamma)) * dt;
  state.speed = Math.max(0, Math.min(TUNE.speedMax, state.speed));

  // Bleed towards the ceiling this height allows rather than clamping to it, so
  // it reads as the air thickening near the ground and thinning as you climb —
  // not as hitting a wall. Diving at a roof scrubs speed off on the way in,
  // which is exactly the flare you want; climbing away hands it back.
  const agl = groundY === null ? 999 : Math.max(0, state.pos.y - groundY);
  const ceiling = MathUtils.lerp(TUNE.groundSpeed, TUNE.speedMax,
                                 MathUtils.clamp(agl / TUNE.speedHeight, 0, 1));
  if (state.speed > ceiling) {
    state.speed += (ceiling - state.speed) * (1 - Math.exp(-dt / TUNE.flareTime));
  }

  // --- heading ------------------------------------------------------------
  // Tilt is a rate, not a position: holding a bank keeps the turn coming, which
  // is how banking an aeroplane works and how tilting a board reads.
  if (board.live) look.yaw -= board.roll * TUNE.boardTurn * dt;
  let delta = ((look.yaw - state.heading + Math.PI) % (Math.PI*2)) - Math.PI;
  if (delta < -Math.PI) delta += Math.PI*2;
  const turn = MathUtils.clamp(delta, -TUNE.turnRate*dt*authority, TUNE.turnRate*dt*authority);
  state.heading += turn;
  state.bank += ((MathUtils.clamp(delta, -1, 1) * TUNE.bankPerTurn) - state.bank) * (1 - Math.exp(-dt/0.22));

  // --- integrate ----------------------------------------------------------
  const forward = headingVector();
  const horizontal = state.speed * Math.cos(state.gamma);
  state.vel.set(forward.x * horizontal, state.speed * Math.sin(state.gamma), forward.z * horizontal);
  state.pitch = state.gamma;
  state.pos.addScaledVector(state.vel, dt);

  // Ground and rooftops. Sampled a few times a second rather than every frame:
  // the city does not move, and a raycast is far and away the most expensive
  // thing here.
  groundAge += dt;
  if (groundAge > 0.06) {
    const found = sampleGround(state.pos);
    if (found !== null) groundY = found;
    groundAge = 0;
  }
  if (groundY !== null) {
    // Perch, do not crash. Landing on a roof is a place to sit and get your
    // breath back, which for a pigeon is most of the point.
    const deck = groundY + 0.35;
    if (state.pos.y < deck) {
      state.pos.y = deck;
      state.vel.y = Math.max(0, state.vel.y);
      // Touching down fast is a scramble, not a perch: the bird keeps its feet
      // only once it is actually slow enough to have landed.
      if (state.speed < TUNE.perchSpeed) {
        state.perched = true;
        state.speed = 0;
        state.gamma = 0;
      } else {
        state.speed *= 0.86;
        state.gamma = Math.max(state.gamma, 0);
      }
    } else if (state.pos.y > deck + 1.2) {
      state.perched = false;
    }
    // Drop the bird onto its starting height once, the first time the ground
    // under it is actually known. Re-applying this every frame is what left it
    // hanging motionless in mid-air until the first wingbeat.
    if (!state.placed) {
      state.pos.y = groundY + 70;
      state.speed = 15;
      state.placed = true;
    }
  }
  if (state.pos.y > 1400) { state.pos.y = 1400; state.vel.y = Math.min(0, state.vel.y); }

  // Pose the bird: nose along the flight path, rolled into the turn.
  bird.group.position.copy(state.pos);
  bird.group.rotation.set(0, 0, 0);
  bird.group.rotateY(-state.heading);
  bird.group.rotateZ(state.pitch);
  bird.group.rotateX(state.bank);

  // Wings run off the same phase the physics does, so what you see beating is
  // literally what is lifting you — at six and a half strokes a second it reads
  // as a blur of effort rather than as an animation.
  // One press is one visible stroke: down hard, then back out to the glide
  // dihedral, where a coasting pigeon actually holds them.
  const stroke = state.stroke < 1 ? Math.sin(state.stroke * Math.PI) : 0;
  for (const { pivot, side } of bird.wings) {
    pivot.rotation.x = side * (0.14 - stroke * 1.55);
  }

  // Chase camera, lagged. It looks where the pointer looks, not where the bird
  // points, so you can watch the city go by while still flying straight.
  const back = new Vector3(Math.cos(look.yaw), 0, Math.sin(look.yaw));
  const want = new Vector3().copy(state.pos)
    .addScaledVector(back, -TUNE.camBack * Math.cos(look.pitch))
    .add(new Vector3(0, TUNE.camUp + Math.sin(look.pitch) * TUNE.camBack, 0));
  camPos.lerp(want, 1 - Math.exp(-dt / TUNE.camLag));
  if (groundY !== null && camPos.y < groundY + 0.8) camPos.y = groundY + 0.8;
  camera.position.copy(camPos);
  camera.lookAt(state.pos.x, state.pos.y + 0.4, state.pos.z);

  updateDroppings(dt);

  const ceilingReadout = ceiling;
  const hud = document.getElementById('hud');
  if (hud) {
    const height = groundY === null ? state.pos.y : state.pos.y - groundY;
    hud.textContent =
      `${board.live ? 'BOARD' : 'keys'}   ${(state.speed*3.6).toFixed(0)} km/h   ` +
      `${height.toFixed(0)} m up   ${(state.stamina).toFixed(0)}% wing   ` +
      (state.perched ? 'perched — press SPACE'
       : state.speed < TUNE.speedStall + 1.5 ? 'STALLING — FLAP'
       : state.beatRate > 0.35 ? `${state.beatRate.toFixed(1)} flaps/s`
       : state.speed > ceilingReadout - 0.4 ? 'low — climb for speed'
       : `gliding, sinking ${sinkRate(state.speed).toFixed(1)} m/s`);
  }
}

/* ---- loop ----------------------------------------------------------------- */

function resize() {
  camera.aspect = innerWidth / innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
  tiles.setResolutionFromRenderer(camera, renderer);
}
addEventListener('resize', resize);
resize();

// Exposed so a frame can be pumped by hand. requestAnimationFrame stops in a
// backgrounded tab, which is exactly when this needs checking from a script.
window.pigeon = { state, look, tiles, scene, camera, renderer, update, beat, drop, respawn, sinkRate, board, recentreBoard, TUNE };

let last = performance.now();
function frame(now) {
  const dt = Math.min(0.05, (now - last) / 1000);
  last = now;
  update(dt);
  camera.updateMatrixWorld();
  if (tilesReady) tiles.update();
  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
