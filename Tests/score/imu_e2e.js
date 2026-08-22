// The IMU toggle, end to end.
//
// Two things to establish: that asking for inertial data produces the nodes on
// a camera that has an IMU and nothing at all on one that does not, and that
// not asking produces nothing either way -- enabling it changes what the camera
// streams, so a device that did not ask must not pay.
//
// And, because an Azure Kinect samples at 1.6kHz: that listening to those nodes
// does not make score sluggish, the way polling the controls from the GUI
// thread did.
//
// Cameras are addressed by bare backend prefix ("orbbec:", "k4a:", ...), which
// every backend reads as "the first camera you can see". That is deliberate:
// the settings object an enumerator hands to JS is an opaque QVariant, and
// copying its properties in JS silently yields {} -- which means an empty
// address, which means every device opens *the first camera on the machine*
// while the report cheerfully prints a different name each time. This test used
// to do exactly that.

var PROTOCOL = "a3bd48ba-f5db-43b7-aa92-2b1a37a88a79";
var REPORT = "/tmp/imu_report.txt";

// Which backends have inertial sensors at all. libfreenect2 has none; a Kinect
// v1's accelerometer is on the motor board, which is opt-in and refused
// outright by a model 1473.
var BACKENDS = [
  { uri: "orbbec:",    imu: true  },
  { uri: "realsense:", imu: true  },
  { uri: "k4a:",       imu: true  },
  { uri: "freenect2:", imu: false },
  { uri: "freenect:",  imu: false },
];

var out = [];
var failures = 0, checks = 0;
function log(s) { out.push(s); Util.writeFile(REPORT, out.join("\n")); }
function check(cond, what) {
  checks++;
  if (!cond) failures++;
  log((cond ? "  PASS  " : "  FAIL  ") + what);
  return cond;
}
function nodesOf(name) {
  var n = [];
  Score.iterateDevice(name, function(a) { n.push(String(a)); });
  return n;
}
function has(nodes, suffix) {
  for (var i = 0; i < nodes.length; i++)
    if (nodes[i].indexOf(suffix) >= 0) return true;
  return false;
}

// ":/imu/", not "/imu/". A camera publishes its inertial *readings* at the
// device root and its inertial *settings* under controls -- "cam:/imu/accel"
// against "cam:/controls/imu/gyro_odr" -- and the loose match cannot tell them
// apart, so an Orbbec with the toggle off looked like it had readings.
function hasImuReadings(nodes) {
  return has(nodes, ":/imu/");
}

var BUDGET_MS = 100;

try {
  log("=== depth camera IMU ===");

  for (var i = 0; i < BACKENDS.length; i++) {
    var be = BACKENDS[i];
    log("");
    log("--- " + be.uri + " ---");

    // 1. Off: no imu nodes at all.
    Score.createDevice("imu_off", PROTOCOL,
                       { Device: be.uri, RGB: true, Depth: true, Imu: false });
    if (Score.device("imu_off") === null) {
      log("    no camera for this backend, skipping");
      continue;
    }
    check(!hasImuReadings(nodesOf("imu_off")), "no imu nodes when the toggle is off");
    Score.removeDevice("imu_off");

    // 2. On.
    Score.createDevice("imu_on", PROTOCOL,
                       { Device: be.uri, RGB: true, Depth: true, Imu: true });
    if (!check(Score.device("imu_on") !== null, "device created with the IMU on"))
      continue;

    var on = nodesOf("imu_on");
    if (be.imu) {
      check(has(on, ":/imu/accel"), "exposes imu/accel");
      check(has(on, ":/imu/gyro"), "exposes imu/gyro");
      check(has(on, ":/imu/temperature"), "exposes imu/temperature");
    } else {
      check(!hasImuReadings(on), "no imu nodes on a camera without one");
    }

    // The streams must still be there.
    check(has(on, "/depth"), "still exposes depth");

    // And listening to a 1.6kHz signal must not stall the caller.
    var t0 = Date.now();
    Score.listenDevice("imu_on");
    var dt = Date.now() - t0;
    check(dt < BUDGET_MS, "listening to everything took " + dt + "ms");

    t0 = Date.now();
    nodesOf("imu_on");
    dt = Date.now() - t0;
    check(dt < BUDGET_MS, "re-walking while the IMU streams took " + dt + "ms");

    Score.removeDevice("imu_on");
  }

  log("");
  log(failures === 0 ? "=== ALL " + checks + " CHECKS PASSED ==="
                     : "=== " + failures + " OF " + checks + " CHECKS FAILED ===");
} catch (e) {
  log("EXCEPTION: " + e);
}
