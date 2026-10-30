import contextlib
import errno
import importlib.util
import io
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

s=importlib.util.spec_from_file_location('fallback',Path(__file__).with_name('fallback-unclaimed.py'))
m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
BOOT='12345678-1234-4234-8234-123456789abc'
OTHER='87654321-4321-4321-8321-cba987654321'


class FallbackTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.claim=Path(self.tmp.name)/'claim'

    @contextlib.contextmanager
    def environment(self, *, identity=True, activity=False, owner=0, group=0):
        real_lstat=os.lstat;real_fstat=os.fstat
        def metadata(s):
            return SimpleNamespace(st_mode=s.st_mode,st_uid=owner,st_gid=group,
                                   st_dev=s.st_dev,st_ino=s.st_ino,st_size=s.st_size)
        # Real local O_EXCL/no-follow creation and reads; only root ownership,
        # platform identity, sleep/proc scan and shutdown are mocked.
        with patch.object(m,'CLAIM',self.claim), \
             patch.object(m,'identity_ok',return_value=identity) as ident, \
             patch.object(m,'boot_id',return_value=BOOT) as boot, \
             patch.object(m,'update_active',return_value=activity) as active, \
             patch.object(m.time,'sleep') as sleep, \
             patch.object(m.os,'fchown') as chown, \
             patch.object(m.os,'fstat',side_effect=lambda fd:metadata(real_fstat(fd))), \
             patch.object(m.os,'lstat',side_effect=lambda p,*a,**kw:metadata(real_lstat(p,*a,**kw))), \
             patch.object(m.os,'fsync') as sync, \
             patch.object(m.subprocess,'run') as reboot, \
             contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
            yield SimpleNamespace(identity=ident,boot=boot,active=active,sleep=sleep,
                                  chown=chown,sync=sync,reboot=reboot)

    def main(self, *args):
        with patch.object(m.sys,'argv',['fallback',*args]):m.main()

    def put_claim(self, data, mode=0o600):
        self.claim.write_bytes(data);self.claim.chmod(mode)

    def test_unclaimed_idle_reserves_and_fsyncs_before_reboot(self):
        with self.environment() as e:
            def requested(*args,**kwargs):
                self.assertEqual(self.claim.read_bytes(),('reboot-requested:'+BOOT+'\n').encode())
                self.assertGreaterEqual(e.sync.call_count,2)
            e.reboot.side_effect=requested
            self.main()
            e.sleep.assert_called_once_with(600)
            self.assertEqual(e.reboot.call_args.args[0],['systemctl','--no-block','reboot'])
            self.assertEqual(e.reboot.call_args.kwargs,{'check':True,'timeout':10})
            self.assertEqual(stat.S_IMODE(self.claim.stat().st_mode),0o600)
            self.assertEqual(e.chown.call_args.args[1:],(0,0))

    def test_wrong_identity_no_claim_no_timer_no_reboot(self):
        for args in [(),('--claim',)]:
            with self.subTest(args=args),self.environment(identity=False) as e:
                with self.assertRaises(SystemExit):self.main(*args)
                e.sleep.assert_not_called();e.reboot.assert_not_called()
                self.assertFalse(self.claim.exists())

    def test_claim_mode_creates_and_reuses_exact_current_boot(self):
        with self.environment() as e:
            self.main('--claim');before=self.claim.stat().st_mtime_ns
            self.assertEqual(self.claim.read_bytes(),(BOOT+'\n').encode())
            self.main('--claim')
            self.assertEqual(self.claim.stat().st_mtime_ns,before)
            e.sleep.assert_not_called();e.active.assert_not_called();e.reboot.assert_not_called()
            self.assertEqual(e.chown.call_count,1) # Reuse never rewrites metadata.

    def test_invalid_claim_contents_refused_not_overwritten(self):
        for data in [b'',BOOT.encode(),(BOOT+'\nextra').encode(),(OTHER+'\n').encode(),
                     ('reboot-requested:'+BOOT+'\n').encode()]:
            with self.subTest(data=data):
                self.put_claim(data)
                with self.environment() as e:
                    with self.assertRaises(SystemExit):self.main('--claim')
                    self.assertEqual(self.claim.read_bytes(),data)
                    e.chown.assert_not_called();e.reboot.assert_not_called()

    def test_bad_claim_mode_owner_group_or_type_refused(self):
        self.put_claim((BOOT+'\n').encode(),0o644)
        with self.environment() as e:
            with self.assertRaises(SystemExit):self.main('--claim')
            e.reboot.assert_not_called()
        self.claim.chmod(0o600)
        for owner,group in [(1000,0),(0,1000)]:
            with self.subTest(owner=owner,group=group),self.environment(owner=owner,group=group):
                with self.assertRaises(SystemExit):self.main('--claim')
        self.claim.unlink();self.claim.mkdir()
        with self.environment():
            with self.assertRaises(SystemExit):self.main('--claim')
        self.claim.rmdir();self.claim.symlink_to(Path(self.tmp.name)/'missing')
        with self.environment():
            with self.assertRaises(SystemExit):self.main('--claim')
        self.assertTrue(self.claim.is_symlink())

    def test_existing_claim_replaced_between_lstat_and_open_is_refused(self):
        self.put_claim((BOOT+'\n').encode())
        replacement=Path(self.tmp.name)/'replacement';replacement.write_bytes((BOOT+'\n').encode());replacement.chmod(0o600)
        real_open=os.open
        def raced_open(path,flags,*args):
            if Path(path)==self.claim and not flags&os.O_CREAT:
                os.replace(replacement,self.claim)
            return real_open(path,flags,*args)
        with self.environment(),patch.object(m.os,'open',side_effect=raced_open):
            with self.assertRaises(SystemExit):self.main('--claim')

    def test_any_existing_claim_holds_fallback_without_parsing(self):
        for data in [b'',b'corrupt',('reboot-requested:'+BOOT+'\n').encode()]:
            self.put_claim(data,0o644)
            with self.environment() as e:
                self.main();e.reboot.assert_not_called();e.active.assert_not_called()
            self.assertEqual(self.claim.read_bytes(),data)
        self.claim.unlink();self.claim.symlink_to(Path(self.tmp.name)/'missing')
        with self.environment() as e:self.main();e.reboot.assert_not_called()

    def test_lstat_permission_error_real_main_fails_closed(self):
        # Regression: old lexists swallowed this error and queued a reboot.
        with self.environment() as e,patch.object(m.os,'lstat',side_effect=PermissionError(errno.EACCES,'denied')):
            with self.assertRaises(SystemExit):self.main()
            e.reboot.assert_not_called()
        self.assertFalse(self.claim.exists())

    def test_non_enoent_lstat_error_is_not_absence(self):
        with self.environment() as e,patch.object(m.os,'lstat',side_effect=NotADirectoryError(errno.ENOTDIR,'bad parent')):
            with self.assertRaises(SystemExit):self.main()
            e.reboot.assert_not_called()

    def test_updater_or_inspection_error_holds(self):
        with self.environment(activity=True) as e:
            self.main();e.reboot.assert_not_called();self.assertFalse(self.claim.exists())
        with self.environment() as e:
            e.active.side_effect=PermissionError(errno.EACCES,'unreadable proc')
            with self.assertRaises(SystemExit):self.main()
            e.reboot.assert_not_called();self.assertFalse(self.claim.exists())

    def test_identity_or_boot_change_before_reservation_holds(self):
        with self.environment() as e:
            e.identity.side_effect=[True,False]
            self.main();e.reboot.assert_not_called();self.assertFalse(self.claim.exists())
        with self.environment() as e:
            e.boot.side_effect=[BOOT,OTHER]
            self.main();e.reboot.assert_not_called();self.assertFalse(self.claim.exists())

    def test_late_claim_wins_during_proc_scan_actual_exclusive_create(self):
        # Regression: claim arrives after the last existence check. The new
        # fallback O_EXCL must lose, rather than queue a reboot as before.
        with self.environment() as e:
            def scan():
                self.main('--claim')
                return False
            e.active.side_effect=scan
            self.main()
            self.assertEqual(self.claim.read_bytes(),(BOOT+'\n').encode())
            e.reboot.assert_not_called()

    def test_fallback_wins_late_controller_is_refused_by_real_claim_mode(self):
        with self.environment() as e:
            scans=0;refused=[]
            def scan():
                nonlocal scans
                scans+=1
                if scans==2:
                    with self.assertRaises(SystemExit):self.main('--claim')
                    refused.append(True)
                return False
            e.active.side_effect=scan
            self.main()
            self.assertEqual(refused,[True]);self.assertEqual(e.reboot.call_count,1)
            self.assertEqual(self.claim.read_bytes(),('reboot-requested:'+BOOT+'\n').encode())

    def test_new_marker_short_writes_are_completed(self):
        real_write=os.write
        def partial(fd,data):return real_write(fd,bytes(data[:3]))
        with self.environment() as e,patch.object(m.os,'write',side_effect=partial):
            self.main('--claim')
            self.assertEqual(self.claim.read_bytes(),(BOOT+'\n').encode());e.reboot.assert_not_called()

    def test_partial_or_fsync_failure_retains_marker_and_never_reboots(self):
        with self.environment() as e,patch.object(m.os,'write',return_value=0):
            with self.assertRaises(SystemExit):self.main()
            e.reboot.assert_not_called()
        self.assertTrue(self.claim.is_file());self.assertEqual(self.claim.read_bytes(),b'')
        self.claim.unlink()
        with self.environment() as e:
            e.sync.side_effect=OSError(errno.EIO,'fsync failed')
            with self.assertRaises(SystemExit):self.main()
            e.reboot.assert_not_called()
        self.assertTrue(self.claim.is_file())

    def test_final_activity_or_check_failure_retains_reservation(self):
        for activity in [[False,True],[False,PermissionError(errno.EACCES,'late proc error')]]:
            with self.subTest(activity=activity),self.environment() as e:
                e.active.side_effect=activity
                if isinstance(activity[-1],Exception):
                    with self.assertRaises(SystemExit):self.main()
                else:self.main()
                e.reboot.assert_not_called()
                self.assertEqual(self.claim.read_bytes(),('reboot-requested:'+BOOT+'\n').encode())
            self.claim.unlink()
        with self.environment() as e:
            e.identity.side_effect=[True,True,False]
            self.main();e.reboot.assert_not_called();self.assertTrue(self.claim.exists())

    def test_reboot_request_failure_retains_intent_and_refuses_claim(self):
        with self.environment() as e:
            e.reboot.side_effect=m.subprocess.TimeoutExpired('systemctl',10)
            with self.assertRaises(SystemExit):self.main()
            with self.assertRaises(SystemExit):self.main('--claim')
            self.assertEqual(e.reboot.call_count,1)
            self.assertEqual(self.claim.read_bytes(),('reboot-requested:'+BOOT+'\n').encode())

    def test_unknown_arguments_do_not_touch_system(self):
        with self.environment() as e:
            with self.assertRaises(SystemExit):self.main('--reboot')
            e.identity.assert_not_called();e.sleep.assert_not_called();e.reboot.assert_not_called()


class ProcAndIdentityTests(unittest.TestCase):
    def test_real_identity_parser_rejects_wrong_or_duplicate_flags(self):
        for uid,host,source,flags,ok in [
            (0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG,True),
            (1000,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG,False),
            (0,'android','/dev/nvme0n1p3',m.FLAG,False),
            (0,'gaokun3-rescue','/dev/nvme0n1p2',m.FLAG,False),
            (0,'gaokun3-rescue','/dev/nvme0n1p3','',False),
            (0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG+' '+m.FLAG,False),
            (0,'gaokun3-rescue','/dev/nvme0n1p3',m.FLAG+' gaokun3.hos_rescue_device=0',False)]:
            with self.subTest(uid=uid,host=host,flags=flags), \
                 patch.object(m.os,'geteuid',return_value=uid),patch.object(m.socket,'gethostname',return_value=host), \
                 patch.object(m.subprocess,'check_output',return_value=source+'\n'),patch.object(m.Path,'read_text',return_value=flags):
                self.assertEqual(m.identity_ok(),ok)

    def test_boot_id_is_canonical(self):
        with patch.object(m.Path,'read_text',return_value=BOOT+'\n'):self.assertEqual(m.boot_id(),BOOT)
        for value in ['corrupt',BOOT.upper()]:
            with self.subTest(value=value),patch.object(m.Path,'read_text',return_value=value):
                with self.assertRaises((ValueError,RuntimeError)):m.boot_id()

    def test_real_proc_parser_matches_block_identity_not_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);proc=root/'123';(proc/'fd').mkdir(parents=True)
            (proc/'cmdline').write_bytes(b'python3\0/home/user/update-isolated-super.py\0--apply\0')
            descriptor=proc/'fd/3';descriptor.symlink_to('/dev/alias-or-deleted-node')
            p7=os.makedev(259,7);disk=os.makedev(259,0);other=os.makedev(259,2)
            target=SimpleNamespace(st_mode=stat.S_IFBLK|0o600,st_rdev=p7)
            real_stat=os.stat
            def inspected(path,*args,**kw):
                name=str(path)
                if name==m.BLOCK_TARGETS[0]:return SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=p7)
                if name==m.BLOCK_TARGETS[1]:return SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=disk)
                if Path(path)==descriptor:
                    if isinstance(target,Exception):raise target
                    return target
                return real_stat(path,*args,**kw)
            with patch.object(m,'PROC',root),patch.object(m.os,'stat',side_effect=inspected), \
                 patch.object(m.os,'readlink',side_effect=AssertionError('must not classify by display path')):
                self.assertTrue(m.update_active())
                (proc/'cmdline').write_bytes(b'sha256sum\0/dev/alias\0')
                self.assertTrue(m.update_active())
                target=SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=disk);self.assertTrue(m.update_active())
                target=SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=other);self.assertFalse(m.update_active())
                target=SimpleNamespace(st_mode=stat.S_IFREG,st_rdev=p7);self.assertFalse(m.update_active())
                target=FileNotFoundError(errno.ENOENT,'closed fd');self.assertFalse(m.update_active())
                target=ProcessLookupError(errno.ESRCH,'exited');self.assertFalse(m.update_active())
                target=PermissionError(errno.EACCES,'unreadable fd')
                with self.assertRaises(PermissionError):m.update_active()

    def test_target_device_inspection_failure_is_not_idle(self):
        for fault in [PermissionError(errno.EACCES,'denied'),FileNotFoundError(errno.ENOENT,'missing')]:
            with self.subTest(fault=fault),patch.object(m.os,'stat',side_effect=fault):
                with self.assertRaises(OSError):m.update_active()
        with patch.object(m.os,'stat',return_value=SimpleNamespace(st_mode=stat.S_IFREG,st_rdev=0)):
            with self.assertRaises(RuntimeError):m.update_active()

    def test_service_keeps_default_boot_and_shutdown_policy(self):
        unit=Path(__file__).with_name('gaokun3-hos-rescue-fallback.service').read_text()
        self.assertIn('ConditionKernelCommandLine='+m.FLAG,unit)
        self.assertIn('After=local-fs.target',unit)
        self.assertIn('ExecStart=/usr/local/sbin/gaokun3-hos-rescue-fallback',unit)
        self.assertNotIn('Restart=',unit);self.assertNotIn('ExecStop=',unit)
        self.assertNotIn('bootctl',unit);self.assertNotIn('reboot --force',unit)


if __name__=='__main__':unittest.main()
