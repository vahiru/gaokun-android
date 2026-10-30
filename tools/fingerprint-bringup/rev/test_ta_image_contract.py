import struct
import unittest
from ta_image_contract import build_request, validate_reply, RESPONSE_BYTES


class ImageContractTests(unittest.TestCase):
    def test_exact_image_request(self):
        self.assertEqual(build_request(),bytes.fromhex('2d80000000000000a01e0000'))

    def test_unsupported_commands_and_dangerous_sizes(self):
        for command in (0x8026,0x8027,0x802b,0x802c,0x8030):
            with self.assertRaises(ValueError):build_request(command)
        for size in (0,12,16,1036,1037,7851,7853,8192):
            with self.assertRaises(ValueError):build_request(response_bytes=size)

    def test_valid_contract_does_not_claim_image_quality_or_authentication(self):
        before=b'\xa5'*RESPONSE_BYTES
        reply=before[:12]+bytes(i%251 for i in range(RESPONSE_BYTES-12))
        result=validate_reply(struct.pack('<III',0x5aa5,0,0),reply,before,transport_status=0)
        self.assertEqual(result['exported_bytes'],7840)
        self.assertFalse(result['image_quality_verified'])
        self.assertFalse(result['template_or_authentication_result'])
        self.assertNotIn('payload',result)

    def test_untouched_short_wrong_ack_and_transport_failure(self):
        before=b'\xa5'*RESPONSE_BYTES;reply=before[:12]+bytes(RESPONSE_BYTES-12)
        ack=struct.pack('<III',0x5aa5,0,0)
        for a,r,b,status in [(ack,before,before,0),(ack,reply[:-1],before,0),
                             (ack,reply,before[:-1],0),(build_request(),reply,before,0),
                             (ack,bytes(12)+reply[12:],before,0),(ack,reply,before,-5)]:
            with self.assertRaises(ValueError):validate_reply(a,r,b,transport_status=status)


if __name__=='__main__':unittest.main()
