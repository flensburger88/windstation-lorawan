// TTN Uplink Payload Formatter (Custom JavaScript) – Windstation Kollund, Payload v1
// [0] Version  [1..2] Akku mV  [3..4] Windfahne roh mV  [5] n
// je Minute 5 Byte: Mittel 0,1 m/s (u16 BE), Böe 0,1 m/s (u16 BE), Richtung 1,5° (u8, 255 = ungültig)
// records[] ist chronologisch, der letzte Eintrag ist die zuletzt abgeschlossene Minute.
function decodeUplink(input) {
  var b = input.bytes;
  if (input.fPort !== 1 || b.length < 6 || b[0] !== 1) {
    return { errors: ["unbekanntes Format (fPort " + input.fPort + ", " + b.length + " Byte)"] };
  }
  var n = b[5];
  if (b.length < 6 + 5 * n) return { errors: ["Payload zu kurz"] };
  var records = [];
  for (var i = 0; i < n; i++) {
    var o = 6 + 5 * i;
    var d = b[o + 4];
    records.push({
      wind_avg_ms: ((b[o] << 8) | b[o + 1]) / 10,
      wind_gust_ms: ((b[o + 2] << 8) | b[o + 3]) / 10,
      wind_dir_deg: d === 255 ? null : d * 1.5
    });
  }
  var data = {
    battery_v: ((b[1] << 8) | b[2]) / 1000,
    vane_raw_mv: (b[3] << 8) | b[4],
    minutes: n,
    records: records
  };
  if (n > 0) {
    var last = records[n - 1];
    data.wind_avg_ms = last.wind_avg_ms;
    data.wind_gust_ms = Math.max.apply(null, records.map(function (r) { return r.wind_gust_ms; }));
    data.wind_dir_deg = last.wind_dir_deg;
  }
  return { data: data };
}
