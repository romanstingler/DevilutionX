#!/usr/bin/env python3

import re
from glob import glob

import polib


def hasUnbalancedBraces(s):
	"""Check for unmatched { or } in a format string, accounting for {{ }} escapes."""
	depth = 0
	i = 0
	while i < len(s):
		if s[i] == '{':
			if i + 1 < len(s) and s[i + 1] == '{':
				i += 2
				continue
			depth += 1
		elif s[i] == '}':
			if i + 1 < len(s) and s[i + 1] == '}':
				i += 2
				continue
			depth -= 1
			if depth < 0:
				return True
		i += 1
	return depth != 0


def validateBraces(translation):
	if translation == '':
		return True
	if hasUnbalancedBraces(translation):
		print(f"\033[31mUnbalanced braces in: {translation}\033[0m")
		return False
	return True


def validateEntry(original, translation):
	if translation == '':
		return True

	# Find fmt arguments in source message
	src_arguments = re.findall(r"{.*?}", original)
	if not src_arguments:
		return True

	# Find fmt arguments in translation
	translated_arguments = re.findall(r"{.*?}", translation)

	# If paramteres are untyped with order, sort so that they still appear equal if reordered
	# Note: This does no hadle cases where the translator reordered arguments where not expected
	# by the source. Or other advanced but valid usages of the fmt syntax
	isOrdered = True
	for argument in src_arguments:
		if not re.search(r"^{\d+}$", argument):
			isOrdered = False
			break

	if isOrdered:
		src_arguments.sort()
		translated_arguments.sort()

	if src_arguments != translated_arguments:
		print(f"\033[36m{original}\033[0m != \033[31m{translation}\033[0m")
		return False

	return True


status = 0

files = glob('Translations/*.po')
for path in sorted(files):
	po = polib.pofile(path)
	print(f"\033[32mValidating {po.metadata['Language']}\033[0m : {po.percent_translated()}% translated")

	for entry in po:
		translations = []
		if entry.msgid_plural:
			translations = list(entry.msgstr_plural.values())
		else:
			translations = [entry.msgstr]

		# Brace balance is always checked, even for fuzzy entries,
		# because un-fuzzying without fixing the braces would ship a crash.
		for translation in translations:
			if not validateBraces(translation):
				status = 255

		if entry.fuzzy:
			continue

		if entry.msgid_plural:
			for translation in translations:
				if not validateEntry(entry.msgid_plural, translation):
					status = 255
			continue

		if not validateEntry(entry.msgid, entry.msgstr):
			status = 255

exit(status)
