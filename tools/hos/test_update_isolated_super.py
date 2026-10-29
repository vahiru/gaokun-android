import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import stat
from types import SimpleNamespace
from contextlib import ExitStack
import tempfile
import unittest
from unittest.mock import patch
import uuid

spec = importlib.util.spec_from_file_location('update_super', Path(__file__).with_name('update-isolated-super.py'))
u = importlib.util.module_from_spec(spec)
spec.loader.exec_module(u)


def entry(name, start, end, guid=None):
    b = bytearray(128)
    b[:16] = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    b[16:32] = uuid.UUID(guid or str(uuid.uuid4())).bytes_le
    struct.pack_into('<QQ', b, 32, start, end)
    n = name.encode('utf-16-le'); b[56:56+len(n)] = n
    return bytes(b)


def fixture():
    target_uuid = str(uuid.uuid4())
    entries = {5: entry('boot_a',814344192,814475263),6: entry('boot_b',814475264,814606335),
               7: entry('super_hos',u.START,u.END,target_uuid)}
    m = {'schema':1, 'disk_sectors':u.SECTORS,'disk_guid':u.DISK_GUID,
         'gpt_primary_sha256':u.digest(b'primary'),'gpt_secondary_sha256':u.digest(b'secondary'),
         'entries':{str(n):b.hex() for n,b in entries.items()},
         'target':{'number':7,'name':'super_hos','uuid':target_uuid,'start':u.START,'end':u.END,'size':u.SIZE,'old_sha256':'a'*64},
         'image_sha256':'b'*64,'protected_partitions':{'boot_a':{'number':5,'sha256':'c'*64},'boot_b':{'number':6,'sha256':'d'*64}},
         'esp_assets':{'loader/loader.conf':'e'*64}}
    gpt = (b'primary',b'secondary',{'guid':u.DISK_GUID,'first':34,'last':u.SECTORS-34},entries)
    return m,gpt


class UpdateTest(unittest.TestCase):
    def test_manifest_pin_duplicate_and_traversal(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'m.json'; m,_=fixture(); p.write_text(json.dumps(m))
            pin=u.digest(p.read_bytes()); self.assertEqual(u.load_manifest(p,pin),m)
            with self.assertRaises(ValueError):u.load_manifest(p,'0'*64)
            m['esp_assets']['../escape']='e'*64; p.write_text(json.dumps(m))
            with self.assertRaises(ValueError):u.load_manifest(p,u.digest(p.read_bytes()))
            p.write_text('{"schema":1,"schema":1}')
            with self.assertRaises(ValueError):u.load_manifest(p,u.digest(p.read_bytes()))

    def test_gpt_exact_entries_and_crc_reader_failure(self):
        m,gpt=fixture()
        with patch.object(u.baseline,'read_gpt',return_value=gpt):
            self.assertEqual(u.verify_gpt(m),gpt[3])
            m['entries']['5']='0'*256
            with self.assertRaises(ValueError):u.verify_gpt(m)
        with patch.object(u.baseline,'read_gpt',side_effect=ValueError('CRC mismatch')):
            with self.assertRaises(ValueError):u.verify_gpt(m)

    def test_boot_geometry_and_target_uuid_cannot_be_reapproved(self):
        m,gpt=fixture(); gpt[3][5]=entry('boot_a',814344193,814475263)
        m['entries']={str(n):b.hex() for n,b in gpt[3].items()}
        with patch.object(u.baseline,'read_gpt',return_value=gpt):
            with self.assertRaises(ValueError):u.verify_gpt(m)
        m,gpt=fixture();m['target']['uuid']=str(uuid.uuid4())
        with patch.object(u.baseline,'read_gpt',return_value=gpt):
            with self.assertRaises(ValueError):u.verify_gpt(m)

    def test_mount_alias_detected_by_devno(self):
        info='25 1 8:7 / /mnt rw - ext4 /dev/disk/by-label/super_hos rw\n'
        self.assertTrue(u.mounted_device(os.makedev(8,7),info))
        self.assertFalse(u.mounted_device(os.makedev(8,8),info))

    def test_bounded_write_and_readback(self):
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'source'; target=Path(d)/'target'
            source.write_bytes(b'A'*91);target.write_bytes(b'B'*91+b'GUARD')
            a=os.open(source,os.O_RDONLY);b=os.open(target,os.O_RDWR)
            try:
                with patch.object(u,'CHUNK',16):u.stream_replace(a,b,91,u.digest(b'A'*91))
                self.assertEqual(target.read_bytes(),b'A'*91+b'GUARD')
            finally:os.close(a);os.close(b)

    def test_short_source_and_mutated_stream_fail(self):
        with tempfile.TemporaryDirectory() as d:
            source=Path(d)/'source';target=Path(d)/'target';source.write_bytes(b'A'*32);target.write_bytes(b'B'*64)
            a=os.open(source,os.O_RDONLY);b=os.open(target,os.O_RDWR)
            try:
                with self.assertRaises(ValueError):u.stream_replace(a,b,64,u.digest(b'A'*64))
                with self.assertRaises(ValueError):u.stream_replace(a,b,32,u.digest(b'Z'*32))
                with patch.object(u,'sha_fd',return_value='0'*64),self.assertRaises(ValueError):
                    u.stream_replace(a,b,32,u.digest(b'A'*32))
            finally:os.close(a);os.close(b)

    def test_environment_rejects_nonroot_and_wrong_rescue(self):
        m,_=fixture()
        with patch.object(u.os,'geteuid',return_value=1000), self.assertRaises(ValueError):u.verify_environment(m)
        with patch.object(u.os,'geteuid',return_value=0),patch.object(u.subprocess,'check_output',return_value='/dev/other\n'),self.assertRaises(ValueError):u.verify_environment(m)

    def test_marker_rejects_partial_success_and_dangling_link(self):
        with tempfile.TemporaryDirectory() as d:
            marker=Path(d)/'started'
            with patch.object(u,'MARKER',marker):
                u.reject_started()
                marker.write_text('{"status":"started"}')
                with self.assertRaises(ValueError):u.reject_started()
                marker.unlink();marker.symlink_to(Path(d)/'missing')
                with self.assertRaises(ValueError):u.reject_started()

    def test_main_preview_never_writes_and_old_hash_blocks_apply(self):
        m,gpt=fixture()
        for apply, old in [(False, 'a'*64), (True, '0'*64)]:
            with self.subTest(apply=apply), ExitStack() as stack:
                stack.enter_context(patch('sys.argv',['update','--manifest','m','--manifest-sha256','f'*64,'--image','i'] + (['--apply'] if apply else [])))
                stack.enter_context(patch.object(u,'load_manifest',return_value=m))
                stack.enter_context(patch.object(u,'verify_environment',return_value=7))
                stack.enter_context(patch.object(u,'verify_gpt',return_value=gpt[3]))
                stack.enter_context(patch.object(u,'verify_protected'))
                opened=stack.enter_context(patch.object(u.os,'open',side_effect=[10,11]))
                stack.enter_context(patch.object(u.os,'close'))
                stack.enter_context(patch.object(u.os,'fstat',side_effect=[
                    SimpleNamespace(st_mode=stat.S_IFREG,st_size=u.SIZE),
                    SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=7),
                    SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=7)]))
                stack.enter_context(patch.object(u,'sha_fd',side_effect=['b'*64,old]))
                stream=stack.enter_context(patch.object(u,'stream_replace'))
                if apply:
                    with self.assertRaises(ValueError):u.main()
                else:
                    stack.enter_context(patch('builtins.print'))
                    u.main()
                    self.assertFalse(opened.call_args_list[1].args[1] & os.O_RDWR)
                stream.assert_not_called()

    def test_pipe_short_reads_truncated_extra_and_wrong_hash(self):
        for payload, expected, fails in [(b'A'*91,u.digest(b'A'*91),False),
                                        (b'A'*90,u.digest(b'A'*91),True),
                                        (b'A'*92,u.digest(b'A'*91),True),
                                        (b'A'*91,u.digest(b'Z'*91),True)]:
            with self.subTest(length=len(payload),fails=fails), tempfile.TemporaryDirectory() as d:
                target=Path(d)/'target';target.write_bytes(b'B'*91+b'GUARD')
                reader,writer=os.pipe();os.write(writer,payload);os.close(writer)
                fd=os.open(target,os.O_RDWR)
                real_read=os.read
                try:
                    # Force legal fragmented pipe reads, including within a write chunk.
                    with patch.object(u.os,'read',side_effect=lambda f,n:real_read(f,min(n,3))),patch.object(u,'CHUNK',16):
                        if fails:
                            with self.assertRaises(ValueError):u.stream_replace(reader,fd,91,expected,sequential=True)
                        else:u.stream_replace(reader,fd,91,expected,sequential=True)
                    self.assertEqual(target.read_bytes()[91:],b'GUARD')
                    if not fails:self.assertEqual(target.read_bytes()[:91],payload)
                finally:os.close(reader);os.close(fd)

    def test_stdin_preview_never_opens_or_reads_source(self):
        m,gpt=fixture()
        with ExitStack() as stack:
            stack.enter_context(patch('sys.argv',['update','--manifest','m','--manifest-sha256','f'*64,'--stdin']))
            stack.enter_context(patch.object(u,'load_manifest',return_value=m))
            stack.enter_context(patch.object(u,'verify_environment',return_value=7))
            stack.enter_context(patch.object(u,'verify_gpt',return_value=gpt[3]))
            stack.enter_context(patch.object(u,'verify_protected'))
            opened=stack.enter_context(patch.object(u.os,'open',return_value=11))
            stack.enter_context(patch.object(u.os,'close'))
            stack.enter_context(patch.object(u.os,'fstat',return_value=SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=7)))
            stack.enter_context(patch.object(u,'sha_fd',return_value='a'*64))
            read=stack.enter_context(patch.object(u.os,'read',side_effect=AssertionError('stdin read')))
            dup=stack.enter_context(patch.object(u.os,'dup',side_effect=AssertionError('stdin dup')))
            printed=stack.enter_context(patch('builtins.print'))
            u.main()
            read.assert_not_called();dup.assert_not_called()
            opened.assert_called_once_with(u.TARGET,os.O_RDONLY|os.O_NOFOLLOW)
            self.assertFalse(json.loads(printed.call_args.args[0])['incoming_bytes_preverified'])

    def test_sysfs_dev_must_match_block_node(self):
        with patch.object(Path,'read_text',return_value='8:7\n'):
            u.verify_sysfs_dev('test',os.makedev(8,7))
            with self.assertRaises(ValueError):u.verify_sysfs_dev('test',os.makedev(8,8))

    def test_apply_success_and_failure_keep_marker_and_audit(self):
        for fail in (False,True):
            m,gpt=fixture()
            with self.subTest(fail=fail),tempfile.TemporaryDirectory() as d,ExitStack() as stack:
                marker=Path(d)/'state'/'started.json'
                stack.enter_context(patch.object(u,'MARKER',marker))
                stack.enter_context(patch('sys.argv',['update','--manifest','m','--manifest-sha256','f'*64,'--stdin','--apply']))
                stack.enter_context(patch.object(u,'load_manifest',return_value=m))
                stack.enter_context(patch.object(u,'verify_environment',return_value=7))
                gptcheck=stack.enter_context(patch.object(u,'verify_gpt',return_value=gpt[3]))
                protected=stack.enter_context(patch.object(u,'verify_protected'))
                real_open,real_close,real_fstat=os.open,os.close,os.fstat
                stack.enter_context(patch.object(u.os,'open',side_effect=lambda path,flags,*args:999 if path==u.TARGET else real_open(path,flags,*args)))
                stack.enter_context(patch.object(u.os,'close',side_effect=lambda fd:None if fd==999 else real_close(fd)))
                stack.enter_context(patch.object(u.os,'fstat',side_effect=lambda fd:SimpleNamespace(st_mode=stat.S_IFBLK,st_rdev=7) if fd==999 else real_fstat(fd)))
                stack.enter_context(patch.object(u,'sha_fd',return_value='a'*64))
                stack.enter_context(patch.object(u.subprocess,'check_output',return_value='/dev/nvme0n1p3\n'))
                stack.enter_context(patch.object(u.os,'dup',side_effect=lambda fd:real_open('/dev/null',os.O_RDONLY)))
                def stream(*args,**kwargs):
                    self.assertTrue(marker.exists())
                    self.assertEqual(json.loads(marker.read_text())['status'],'started')
                    if fail:raise ValueError('simulated partial stream')
                stack.enter_context(patch.object(u,'stream_replace',side_effect=stream))
                stack.enter_context(patch('builtins.print'))
                if fail:
                    with self.assertRaises(ValueError):u.main()
                else:u.main()
                self.assertTrue(marker.exists())
                self.assertEqual(protected.call_count,3)
                self.assertEqual(gptcheck.call_count,3)
                with self.assertRaises(ValueError):u.reject_started()

    def test_protected_esp_loader_changes_rejected(self):
        m,gpt=fixture()
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);disk=root/'disk';disk.write_bytes(b'')
            esp=root/'esp';(esp/'loader').mkdir(parents=True);(esp/'loader/loader.conf').write_text('reviewed')
            m['esp_assets']={'loader/loader.conf':u.digest(b'reviewed')}
            with patch.object(u,'DISK',disk),patch.object(u,'ESP',esp),patch.object(u,'sha_fd',side_effect=['c'*64,'d'*64]):u.verify_protected(m,gpt[3])
            (esp/'loader/new.conf').write_text('unexpected')
            with patch.object(u,'DISK',disk),patch.object(u,'ESP',esp),patch.object(u,'sha_fd',side_effect=['c'*64,'d'*64]),self.assertRaises(ValueError):u.verify_protected(m,gpt[3])

if __name__=='__main__':unittest.main()
