// Host test for the PrusaLink material JSON parsers (dc_prusa_parse.h):
// dc_prusa_parse_job_material (/api/v1/job -> file.meta.filament_type) and
// dc_prusa_parse_printer_material (legacy /api/printer -> telemetry.material).
// Exercised against real Buddy/PrusaLink payload shapes: scalar + per-tool job
// metadata, the real XL "no file.meta" case, the "---" no-filament sentinel,
// missing fields, malformed JSON, and output truncation.
#include "dc_prusa_parse.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

// Expect job parser: want_ok, and (when ok) want_mat equals the token.
static void job(const char *name, const char *json, int want_ok, const char *want_mat)
{
    char out[DC_PRUSA_MAT_MAX] = "SENTINEL";
    bool ok = dc_prusa_parse_job_material(json, out, sizeof(out));
    int good = (ok == (bool)want_ok) &&
               (!want_ok ? strcmp(out, "SENTINEL") == 0        // false must not write
                         : strcmp(out, want_mat) == 0);
    if (!good) fails++;
    printf("[%s] job %-22s ok=%d out=\"%s\"\n", good ? "PASS" : "FAIL", name, ok, out);
}

static void printer(const char *name, const char *json, int want_ok, const char *want_mat)
{
    char out[DC_PRUSA_MAT_MAX] = "SENTINEL";
    bool ok = dc_prusa_parse_printer_material(json, out, sizeof(out));
    int good = (ok == (bool)want_ok) &&
               (!want_ok ? strcmp(out, "SENTINEL") == 0
                         : strcmp(out, want_mat) == 0);
    if (!good) fails++;
    printf("[%s] printer %-18s ok=%d out=\"%s\"\n", good ? "PASS" : "FAIL", name, ok, out);
}

int main(void)
{
    // ---- /api/v1/job (modern v1) ----
    // Single-toolhead: scalar file.meta.filament_type.
    job("scalar PLA",
        "{\"file\":{\"name\":\"a.gco\",\"meta\":{\"filament_type\":\"PLA\"}}}",
        1, "PLA");

    // Multi-toolhead (XL): pick the PRINTED slot by usage, not slot 0.
    job("per-tool used slot1",
        "{\"file\":{\"meta\":{"
        "\"filament_type per tool\":[\"PLA\",\"PETG\",\"ABS\",\"PC\"],"
        "\"filament used [mm] per tool\":[0,939.9,0,0]}}}",
        1, "PETG");
    job("per-tool used slot3",
        "{\"file\":{\"meta\":{"
        "\"filament_type per tool\":[\"PLA\",\"PETG\",\"ABS\",\"PP\"],"
        "\"filament used [mm] per tool\":[0,0,0,88.1]}}}",
        1, "PP");
    // Per-tool present but no usage array -> slot 0 (pick falls back to primary).
    job("per-tool no usage",
        "{\"file\":{\"meta\":{\"filament_type per tool\":[\"ASA\",\"PETG\"]}}}",
        1, "ASA");
    // Per-tool array but scalar also present: per-tool (used) wins.
    job("per-tool over scalar",
        "{\"file\":{\"meta\":{\"filament_type\":\"PLA\","
        "\"filament_type per tool\":[\"X\",\"ABS\"],"
        "\"filament used [mm] per tool\":[0,10]}}}",
        1, "ABS");

    // The REAL Prusa XL case: a USB-sent file has NO file.meta at all -> no material
    // (so dc_prusa falls through to the legacy /api/printer endpoint).
    job("no meta (USB)",
        "{\"id\":692,\"state\":\"PRINTING\",\"file\":{\"name\":\"HATCH.GCO\","
        "\"display_name\":\"hatch_PP_XL5IS.gcode\",\"path\":\"/usb\"}}",
        0, NULL);
    job("empty meta",       "{\"file\":{\"meta\":{}}}", 0, NULL);
    job("meta empty type",  "{\"file\":{\"meta\":{\"filament_type\":\"\"}}}", 0, NULL);
    job("no file",          "{\"state\":\"PRINTING\"}", 0, NULL);
    job("malformed",        "{\"file\":{\"meta\":{", 0, NULL);
    job("not json",         "totally not json", 0, NULL);
    job("null",             NULL, 0, NULL);

    // ---- /api/printer (legacy OctoPrint-compatible fallback) ----
    printer("telemetry PP",
            "{\"telemetry\":{\"temp-bed\":80.0,\"material\":\"PP\"},"
            "\"state\":{\"text\":\"Printing\"}}",
            1, "PP");
    printer("telemetry PETG", "{\"telemetry\":{\"material\":\"PETG\"}}", 1, "PETG");
    // "---" = no filament loaded -> reject.
    printer("no filament ---", "{\"telemetry\":{\"material\":\"---\"}}", 0, NULL);
    printer("empty material",  "{\"telemetry\":{\"material\":\"\"}}", 0, NULL);
    printer("no material",     "{\"telemetry\":{\"temp-bed\":60.0}}", 0, NULL);
    printer("no telemetry",    "{\"state\":{\"text\":\"Idle\"}}", 0, NULL);
    printer("malformed",       "{\"telemetry\":", 0, NULL);
    printer("null",            NULL, 0, NULL);

    // Output truncation: a material longer than the buffer is written safely.
    {
        char out[6] = {0};   // room for 5 chars + NUL
        const char *j = "{\"telemetry\":{\"material\":\"PCABSXT\"}}";
        bool ok = dc_prusa_parse_printer_material(j, out, sizeof(out));
        int good = ok && strcmp(out, "PCABS") == 0 && out[5] == '\0';
        if (!good) fails++;
        printf("[%s] printer truncation        out=\"%s\"\n", good ? "PASS" : "FAIL", out);
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
