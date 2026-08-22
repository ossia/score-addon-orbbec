// Reconnect behaviour: a camera that has been disconnected must let go of the
// hardware, and a reconnect must end up with the same tree it started with.
//
// This is the Azure Kinect symptom in test form: k4a_device_open fails for a
// device this process already has open, so a reconnect that opened before
// releasing produced a device with no nodes -- and reported success.

var PROTOCOL = "a3bd48ba-f5db-43b7-aa92-2b1a37a88a79";
var REPORT = "/tmp/reconnect_report.txt";

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

try {
  log("=== depth camera reconnect ===");

  var en = Score.enumerateDevices(PROTOCOL);
  var cams = [];
  if (en) {
    en.enumerate = true;
    var list = en.devices;
    for (var i = 0; i < list.length; i++)
      cams.push({ name: list[i].name, category: list[i].category, settings: list[i].settings });
  }
  check(cams.length > 0, "at least one camera enumerated");

  for (var i = 0; i < cams.length; i++) {
    var a = "rc_a_" + i, b = "rc_b_" + i;
    log("");
    log("--- [" + cams[i].category + "] " + cams[i].name + " ---");

    Score.createDevice(a, PROTOCOL, cams[i].settings);
    if (!check(Score.device(a) !== null, "first device created")) continue;
    var first = nodesOf(a);
    check(first.length > 0, "first device has " + first.length + " nodes");

    // A second device on the same camera. libk4a and libfreenect claim the USB
    // interface exclusively so this legitimately fails for them; what must not
    // happen either way is the first device losing its streams.
    Score.createDevice(b, PROTOCOL, cams[i].settings);
    var second = nodesOf(b);
    log("    a second device on the same camera opened " + second.length + " nodes"
        + (second.length === 0 ? "  (exclusive claim -- expected)" : ""));
    var stillFirst = nodesOf(a);
    check(stillFirst.length === first.length,
          "the first device kept its " + first.length + " nodes");
    Score.removeDevice(b);

    // The camera has to be free the instant the last device is gone -- not
    // 100ms later when GfxContext gets round to deleting the graphics nodes.
    // Reopening immediately is the whole point of the test.
    Score.removeDevice(a);
    Score.createDevice(a, PROTOCOL, cams[i].settings);
    var again = nodesOf(a);
    check(again.length === first.length,
          "reopens immediately after removal (" + again.length + " nodes)");
    Score.removeDevice(a);
  }

  log("");
  log(failures === 0 ? "=== ALL " + checks + " CHECKS PASSED ==="
                     : "=== " + failures + " OF " + checks + " CHECKS FAILED ===");
} catch (e) {
  log("EXCEPTION: " + e);
}
