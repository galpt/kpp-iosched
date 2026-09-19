# kpp-iosched

KPP is a separate multiqueue scheduler cloned from Kyber. It shows as kpp next to kyber in the scheduler file for each device, and the original Kyber code stays untouched.

The baseline is Linux 7.3-rc3. Backport notes for older trees live in the patch header.

## Layout

- `patches/7.3/0001-kpp-add-KPP-scheduler.patch` holds the distributable patch. It applies with patch or with git apply.
- `src/kpp-iosched.c` holds the new block source.
- `src/kpp-trace.h` holds the new trace header.

## What stays the same

All constants match Kyber, and there are no new tunables. Queue depths stay at 256 for reads, 128 for writes, 64 for discards, and 16 for other types. Latency targets stay at 2ms for reads, 10ms for writes, and 5s for discards. Batch sizes stay at 16 for reads, 8 for writes, 1 for discards, and 1 for other types.

## What changes

Only two small deltas ride on top of Kyber behavior.

First, insert position uses bounded LIFO reuse. Storage stays FIFO, order within each domain is not persistence order, and correctness still rests with the flush and host context paths. Requests are never dropped and never move across domains.

Second, dispatch and timer work stay capped at constant cost, so each stays O(1). Dispatch splices at most 8 busy queues per pass and resumes with a cursor. The timer sums at most 8 CPUs per run and rearms when more remain. Targets stay unchanged.

## Build

To build a whole kernel, apply the patch in a clean 7.3-rc3 tree and enable the KPP option. Then build as usual.

```sh
cd /path/to/linux-7.3-rc3
patch -p1 -N -F 10 --dry-run < /path/to/kpp-iosched/patches/7.3/0001-kpp-add-KPP-scheduler.patch
patch -p1 -N -F 10 < /path/to/kpp-iosched/patches/7.3/0001-kpp-add-KPP-scheduler.patch
make -j$(nproc)
```

Enable CONFIG_MQ_IOSCHED_KPP in menuconfig. The default is on. Git apply check also passes.

## Fast rebuild

After patching, you can rebuild only the module without a full kernel build. This needs a patched tree with the module option set as module, not a pristine tree.

```sh
cd /path/to/linux-7.3-rc3/block
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo insmod kpp-iosched.ko
echo kpp > /sys/block/<dev>/queue/scheduler
```

## Checks

Output was validated with checkpatch in the 7.3-rc3 tree. The block source reports the same one error and six warnings as Kyber, and the trace header reports the same 24 errors as the Kyber trace header for macro style. There are zero new reports from the KPP deltas.

Run these commands from the tree root.

```sh
perl scripts/checkpatch.pl --no-tree --file block/kpp-iosched.c
perl scripts/checkpatch.pl --patch --strict patches/7.3/0001-kpp-add-KPP-scheduler.patch
```

## Use

After build, the scheduler file lists kpp next to kyber. You can echo kpp or echo kyber to switch at runtime. Fallback is runtime selection of kyber or removal of the two wiring lines and the two new files.

## Verification still required

Build still needs your toolchain check with a full 7.3 build and the object build. Bring up still needs null block tests, fio p99, blktrace cadence, lockdep, and scale checks on large context and CPU counts. Backport series for older trees still need separate review if you need them.

## Credits

Credits go to the developers who implemented the original Kyber. That is Omar Sandoval in 2017 at Facebook, plus the Linux block community who reviewed and maintained it. The Kyber file header carries Copyright 2017 Facebook with no individual name in the file, but history records Omar as author. KPP only clones that work with small deltas.
