import unittest
from hop_linux.panel import default_language, labels


class PanelTextTests(unittest.TestCase):
    def test_bilingual_strings_have_consistent_fields(self):
        zh, en = labels("zh"), labels("en")
        self.assertEqual(set(zh), set(en))
        self.assertTrue(all(isinstance(v, str) and v for v in (*zh.values(), *en.values())))
        self.assertIn("预览", zh["status"])
        self.assertIn("preview", en["status"].lower())
    def test_locale_fallback_and_explicit_language(self):
        self.assertEqual(default_language({"LANG": "zh_CN.UTF-8"}), "zh")
        self.assertEqual(default_language({"LANG": "en_US.UTF-8"}), "en")
        self.assertEqual(default_language({"LC_ALL": "zh_Hans", "LANG": "en_US"}), "zh")
        self.assertEqual(default_language({}), "en")
        with self.assertRaises(ValueError): labels("fr")
