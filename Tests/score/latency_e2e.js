// Nothing the camera does may block a caller.
//
// Unfolding a node in the explorer makes score listen to every child at once,
// and each listen reaches protocol_base::observe(). If that -- or the pull that
// follows it -- talks to the camera on the calling thread, the GUI stalls for
// as long as forty USB transfers take, twice a second. On a networked camera it
// is worse: each read is a TCP round trip.
//
// So: time the operations a user's click turns into. They have to be
// instantaneous, whatever the camera is.

var PROTOCOL = "a3bd48ba-f5db-43b7-aa92-2b1a37a88a79";
var REPORT = "/tmp/latency_report.txt";

var out = [];
var failures = 0, checks = 0;
function log(s) { out.push(s); Util.writeFile(REPORT, out.join("\n")); }
function check(cond, what) {
  checks++;
  if (!cond) failures++;
  log((cond ? "  PASS  " : "  FAIL  ") + what);
  return cond;
}

// A click that takes longer than this is one the user feels.
var BUDGET_MS = 100;

function timed(what, fn) {
  var t0 = Date.now();
  fn();
  var dt = Date.now() - t0;
  check(dt < BUDGET_MS, what + " took " + dt + "ms (budget " + BUDGET_MS + "ms)");
  return dt;
}

try {
  log("=== depth camera responsiveness ===");

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
    var name = "lat_" + i;
    log("");
    log("--- [" + cams[i].category + "] " + cams[i].name + " ---");

    // Creating the device does open the camera, which is genuinely slow on some
    // of them, so that one is not part of the budget -- but it must not also be
    // paying for a read of every control.
    var t0 = Date.now();
    Score.createDevice(name, PROTOCOL, cams[i].settings);
    log("    open + build tree: " + (Date.now() - t0) + "ms");
    if (!check(Score.device(name) !== null, "device created")) continue;

    var n = 0;
    timed("walking the tree", function() {
      Score.iterateDevice(name, function(a) { n++; });
    });
    log("    " + n + " nodes");

    // What unfolding a node does, for the whole tree at once.
    timed("listening to every node", function() { Score.listenDevice(name); });

    // And again, now that the poll thread is running.
    timed("re-walking while polling", function() {
      Score.iterateDevice(name, function(a) {});
    });

    Score.removeDevice(name);
  }

  log("");
  log(failures === 0 ? "=== ALL " + checks + " CHECKS PASSED ==="
                     : "=== " + failures + " OF " + checks + " CHECKS FAILED ===");
} catch (e) {
  log("EXCEPTION: " + e);
}
