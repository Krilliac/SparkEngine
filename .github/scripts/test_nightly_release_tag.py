import unittest

from nightly_release_tag import TAG_RE, make_tag


class NightlyReleaseTagTests(unittest.TestCase):
    def test_tag_is_unique_per_run_attempt_and_source(self):
        first = make_tag("123", "1", "A" * 40)
        retry = make_tag("123", "2", "A" * 40)
        other_source = make_tag("123", "1", "B" * 40)
        self.assertEqual(first, "nightly-123-1-aaaaaaaaaaaa")
        self.assertNotEqual(first, retry)
        self.assertNotEqual(first, other_source)
        self.assertRegex(first, TAG_RE)

    def test_rejects_untrusted_identity(self):
        for args in (("0", "1", "a" * 40), ("1", "0", "a" * 40),
                     ("1", "1", "not-a-sha"), ("1", "1", "a" * 39)):
            with self.subTest(args=args):
                with self.assertRaises(ValueError):
                    make_tag(*args)


if __name__ == "__main__":
    unittest.main()
