# Read-only thermal contract probe

Build target: `gaokun3-thermal-contract-probe` (vendor, static AIDL thermal V3).
This standalone diagnostic is not part of PRODUCT_PACKAGES and has no init RC.
Build it explicitly with the existing Android tree; retain build output in cache.

It checks the already-running thermal service without starting a lazy service,
prints status descriptions, sensor names/readings/severities and hot/cold arrays,
and calls only read methods. It never registers callbacks, injects temperatures,
changes thresholds, starts/stops services or reboots.

Checks include a nonempty valid SKIN reading set, matching valid seven-entry
threshold arrays, EX_ILLEGAL_ARGUMENT for forecast(-1/INT_MIN), and successful
finite forecasts within -40..150 degrees Celsius for 0/60 seconds. Negative
Celsius is valid. NaN threshold entries are valid unavailable levels. The probe
does not assert that a forecast equals a separately sampled current temperature.
Any failed check returns exit code 1; complete success returns 0. Failure to
access an existing service also returns 1. Readings and thresholds are separate
snapshots, so disappearance during the probe can legitimately fail validation.

Use a caller context permitted to read the vendor thermal Binder service. This
probe does not alter SELinux policy or treat an access denial as a passing test.
