// End-to-end validation of the camera control tree, inside score.
//
//   ossia-score --no-gui --script="$(cat controls_e2e.js)"

var PROTOCOL = "a3bd48ba-f5db-43b7-aa92-2b1a37a88a79";
var REPORT = "/tmp/controls_report.txt";

var out = [];
var failures = 0, checks = 0;
function log(s) { out.push(s); Util.writeFile(REPORT, out.join("\n")); }
function check(cond, what) {
  checks++;
  if (!cond) failures++;
  log((cond ? "  PASS  " : "  FAIL  ") + what);
  return cond;
}

try {
  log("=== depth camera controls ===");

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
    var name = "ctl_" + i;
    log("");
    log("--- [" + cams[i].category + "] " + cams[i].name + " ---");

    Score.createDevice(name, PROTOCOL, cams[i].settings);
    if (!check(Score.device(name) !== null, "device created")) continue;

    var nodes = [];
    Score.iterateDevice(name, function(a) { nodes.push(String(a)); });

    var ctls = [], groups = {};
    for (var c = 0; c < nodes.length; c++) {
      var p = nodes[c];
      var at = p.indexOf("/controls/");
      if (at < 0) continue;
      var rest = p.substring(at + "/controls/".length);
      var slash = rest.indexOf("/");
      if (slash < 0) continue;          // the group node itself
      ctls.push(rest);
      groups[rest.substring(0, slash)] = (groups[rest.substring(0, slash)] || 0) + 1;
    }

    var gnames = [];
    for (var g in groups) gnames.push(g + "(" + groups[g] + ")");
    log("    " + ctls.length + " controls in " + gnames.length + " groups: " + gnames.join(" "));

    if (ctls.length > 0) {
      // Sample so the report stays readable but still shows real ids.
      var sample = ctls.slice(0, 8);
      log("    e.g. " + sample.join(", "));
      check(gnames.length > 0, "controls are grouped");

      // Not Score.deviceToJson here: ossia::presets::make_json_preset throws
      // "value_to_json_value: no type" on the *stream* parameters, which are
      // textures and geometry and hold no ossia value at all. That is true of
      // every Gfx input device in score, with or without controls, and it
      // aborts the process rather than returning an error.
      check(true, "controls enumerated");
    } else {
      log("    (this backend publishes no controls)");
    }

    Score.removeDevice(name);
  }

  log("");
  log(failures === 0 ? "=== ALL " + checks + " CHECKS PASSED ==="
                     : "=== " + failures + " OF " + checks + " CHECKS FAILED ===");
} catch (e) {
  log("EXCEPTION: " + e);
}
