#!/usr/bin/env python3
"""Every native method a Java class declares has a C++ symbol JNI can find.

JNI binds a `native` method to a C function by *name*: a method `m` on class
`p.q.C` resolves to `Java_p_q_C_m`, and nothing checks the two agree until the
first call throws `UnsatisfiedLinkError` on a device.

This tree shipped that mistake. The application id was renamed from
`org.qtproject.example.hydra` to `se.vibes.hydra`; the Java moved, and so did
the class paths C++ passes to `QJniObject` -- those are string literals
containing `org/qtproject/example/hydra`, which a grep for the old id finds.
The seven JNI entry points did not move, because in a function name the
separator is `_` rather than `/` and the same grep does not match. The result
built, packaged, installed and signed, and every native call in the WebView
backend would have failed: no url reporting, no request filtering, no script
bridges, no external links, no file picker.

Nothing caught it because nothing could. The Java compiles without the C++,
the C++ compiles without the Java, and the two are joined at runtime by string
equality. This is that equality, checked at rest.

**The arguments are joined by nothing at all, which is worse.** JNI binds on
the name alone, so a C++ entry point whose parameters disagree with the Java
declaration is still bound and still called -- with the arguments read as
whatever the C++ says they are. There is no `UnsatisfiedLinkError` for that
and no diagnostic anywhere: an `int` arriving where a `jstring` is declared
is a pointer built from a small integer, dereferenced. So the parameter
types and the return type are compared here too, not only the name.

The comparison is exact for primitives and for `String`, and permissive one
way for object types: Java `WebView` may arrive as `jobject`, which is what
every entry point here actually writes. That keeps the catches worth having
-- an argument added on one side, two swapped, `int` against `String` --
without flagging a widening that is legal at the ABI.

Text only -- no Android SDK, no NDK, no device -- so it runs in seconds
anywhere, which is what makes it worth running every time.

**It carries its own controls and runs them first.** A checker over a tree
that is already clean is a green light with no demonstrated ability to be
anything else, and this one has been clean since it was written. The samples
in `self_test` are deliberately broken in each way the checker claims to
catch; if any of them is classified wrongly the tool says so and refuses to
report on the real tree at all, because a result underneath a broken
instrument means nothing.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JAVA = os.path.join(ROOT, "android", "src")
CPP = os.path.join(ROOT, "src")

RE_PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.M)
# The return type and the parameter list as well as the name, because the
# parameters are the half JNI does not check for anybody.
RE_NATIVE = re.compile(r"\bnative\s+([\w.<>\[\]]+)\s+(\w+)\s*\(([^)]*)\)", re.M)
# **The return type is on the line above, in this tree and in most.** The real
# entry points are written `extern "C" JNIEXPORT void JNICALL` and then the
# symbol at column 0 on the next line, so a pattern that wants the type on the
# same line matches nothing -- which is what the first version of this did,
# reporting all 23 methods missing. The samples in the control were written on
# one line and agreed with it, which is a stand-in reproducing the half of the
# real thing its author had in view.
RE_SYMBOL = re.compile(
  r'((?:extern\s+"C"\s+)?(?:JNIEXPORT\s+)?[\w:*&]+(?:\s+JNICALL)?)\s+'
  r"(Java_\w+)\s*\(([^)]*)\)", re.M)

# Java spelling to the JNI type an entry point must declare. Anything not here
# is a class, and a class arrives as `jobject`.
JNI_OF = {
	"boolean": "jboolean", "byte": "jbyte",   "char":   "jchar",
	"short":   "jshort",   "int":  "jint",    "long":   "jlong",
	"float":   "jfloat",   "double": "jdouble",
	"void":    "void",     "String": "jstring",
}


def jni_type(java):
	"""The JNI spelling for a Java type, arrays included."""
	java = java.strip()
	if java.endswith("[]"):
		base = java[:-2].strip()
		if base in JNI_OF and base != "String":
			return JNI_OF[base] + "Array"
		return "jobjectArray"
	return JNI_OF.get(java, "jobject")


def compatible(want, got):
	"""Whether a C++ parameter spelling `got` may stand for Java's `want`.

	Exact for primitives and for String, because those are the mistakes worth
	catching and none of them is a widening. An object type may be written as
	the general `jobject`, which is what every entry point in this tree does,
	and refusing that would be a checker demanding a spelling nobody uses.
	"""
	want, got = want.strip(), got.strip()
	if want == got:
		return True
	# An array of objects may be written as the bare `jobject` it is.
	return want == "jobjectArray" and got == "jobject"


def parse_java(text, cls, where_of):
	"""{symbol: (where, [jni param types], jni return type)} from one file's text.

	`where_of` turns a method name into the string used in messages, so this
	can be fed a real file or a sample.
	"""
	out = {}
	pkg = RE_PACKAGE.search(text)
	if not pkg:
		return out
	for ret, method, args in RE_NATIVE.findall(text):
		# JNI's short form. An overloaded native method needs the long form
		# with the argument signature appended; none here are overloaded, and
		# the check below reports it if that changes.
		symbol = "Java_%s_%s_%s" % (pkg.group(1).replace(".", "_"), cls, method)
		params = [jni_type(a.strip().split()[0])
		           for a in args.split(",") if a.strip()]
		out[symbol] = (where_of(pkg.group(1), cls, method), params, jni_type(ret))
	return out


def java_natives():
	"""{expected symbol: (where, params, return)} for every native method."""
	wanted = {}
	if not os.path.isdir(JAVA):
		return wanted
	for base, _, names in os.walk(JAVA):
		for name in names:
			if not name.endswith(".java"):
				continue
			path = os.path.join(base, name)
			with open(path) as fh:
				text = fh.read()
			rel = os.path.relpath(path, ROOT)
			found = parse_java(
			  text, name[:-5],
			  lambda p, c, m, rel=rel: "%s: %s.%s.%s" % (rel, p, c, m))
			for symbol, info in found.items():
				if symbol in wanted:
					print("two native methods want one symbol: %s" % symbol)
					print("    %s" % wanted[symbol][0])
					print("    %s" % info[0])
					return None
				wanted[symbol] = info
	return wanted


def parse_cpp(text, where):
	"""{symbol: (where, [param types past JNIEnv and this], return type)}."""
	out = {}
	for ret, symbol, args in RE_SYMBOL.findall(text):
		# The declared type of each parameter, with the parameter's own name
		# dropped: `jstring url` is a `jstring`.
		params = [re.sub(r"\s+\w+$", "", a.strip()).strip()
		           for a in args.split(",") if a.strip()]
		# Every entry point takes the environment and the object or class it
		# was called on; those are JNI's and are not part of the comparison.
		# `JNIEXPORT void JNICALL` -- the decorations are the platform's and
		# the type is what is left once they are dropped. Taking the last word
		# instead reads the return type as `JNICALL`, which the control below
		# caught on its first run.
		words = [w for w in ret.replace("*", " * ").split()
		          if w not in ("JNIEXPORT", "JNICALL", "extern", '"C"', "static")]
		out[symbol] = (where, params[2:], words[-1].strip() if words else "")
	return out


def cpp_symbols():
	"""{symbol: (file, params, return)} for every JNI entry point in src/."""
	found = {}
	for name in sorted(os.listdir(CPP)):
		if not name.endswith(".cpp"):
			continue
		with open(os.path.join(CPP, name)) as fh:
			found.update(parse_cpp(fh.read(), name))
	return found


# --- the other direction: C++ calling into Java -----------------------------
#
# `QJniObject::callStaticMethod<T>(class, "name", "(descriptor)ret", ...)` is
# joined to the Java by string equality exactly as the entry points are, and
# nothing checked it either. A renamed method or a changed parameter throws a
# Java exception at the moment somebody uses the feature, on a device, with
# nothing at build time to say so -- and this is the direction the incident
# that produced this tool actually broke, since the class paths are string
# literals carrying the old application id.

RE_IMPORT = re.compile(r"^\s*import\s+([\w.]+)\s*;", re.M)
RE_JAVA_STATIC = re.compile(
  r"\b(?:public|private|protected)?\s*static\s+(?:final\s+)?"
  r"([\w.<>\[\]]+)\s+(\w+)\s*\(([^)]*)\)\s*\{", re.M)
RE_CPP_CALL = re.compile(
  r"callStaticMethod<\s*[\w:]+\s*>\s*\(\s*([\w:]+|\"[^\"]+\")\s*,\s*"
  r"\"([^\"]+)\"\s*,\s*\"([^\"]+)\"", re.S)
RE_CLASS_CONST = re.compile(r"(\w+)\s*=\s*\"([\w]+(?:/[\w]+)+)\"")

DESCRIPTOR_OF = {
	"void": "V", "boolean": "Z", "byte": "B", "char": "C", "short": "S",
	"int": "I", "long": "J", "float": "F", "double": "D",
	"String": "Ljava/lang/String;", "Object": "Ljava/lang/Object;",
	# `java.lang` needs no import, so these never appear in the import map and
	# would otherwise read as unresolvable.
	"Runnable": "Ljava/lang/Runnable;", "CharSequence": "Ljava/lang/CharSequence;",
	"Integer": "Ljava/lang/Integer;", "Long": "Ljava/lang/Long;",
	"Boolean": "Ljava/lang/Boolean;", "Throwable": "Ljava/lang/Throwable;",
}


def descriptor(java, imports):
	"""The JVM descriptor for a Java type, or None when it cannot be resolved.

	None rather than a guess: a checker that invents a descriptor reports
	agreement it never established, which is worse than saying it could not
	look.
	"""
	java = java.strip()
	if java.endswith("[]"):
		inner = descriptor(java[:-2], imports)
		return None if inner is None else "[" + inner
	if java in DESCRIPTOR_OF:
		return DESCRIPTOR_OF[java]
	if java in imports:
		return "L" + imports[java].replace(".", "/") + ";"
	return None


def parse_java_statics(text, class_path):
	"""{class path: {method: [descriptor, ...]}} for one file's static methods."""
	imports = {}
	for full in RE_IMPORT.findall(text):
		imports[full.rsplit(".", 1)[-1]] = full
	out = {}
	for ret, method, args in RE_JAVA_STATIC.findall(text):
		params = []
		for arg in args.split(","):
			arg = arg.strip()
			if not arg:
				continue
			# **Split on any whitespace, not on a space.** A parameter list
			# wrapped across lines carries tabs, and splitting on " " took the
			# whole of "long<tab>id" as a type nothing could resolve -- every
			# one of the 25 calls then read as unresolvable. Measured while
			# writing this.
			params.append(re.split(r"\s+", arg)[-2])
		sig = "".join(descriptor(p, imports) or "?" for p in params)
		out.setdefault(method, []).append(
		  "(%s)%s" % (sig, descriptor(ret, imports) or "?"))
	return {class_path: out}


def java_statics():
	"""Every static method of every class under android/src, by class path."""
	classes = {}
	if not os.path.isdir(JAVA):
		return classes
	for base, _, names in os.walk(JAVA):
		for name in names:
			if not name.endswith(".java"):
				continue
			with open(os.path.join(base, name)) as fh:
				text = fh.read()
			pkg = RE_PACKAGE.search(text)
			if not pkg:
				continue
			path = pkg.group(1).replace(".", "/") + "/" + name[:-5]
			classes.update(parse_java_statics(text, path))
	return classes


def parse_cpp_calls(text, where):
	"""[(where, class path, method, descriptor)] for one file's calls out."""
	consts = dict(RE_CLASS_CONST.findall(text))
	out = []
	for cls, method, sig in RE_CPP_CALL.findall(text):
		out.append((where, consts.get(cls, cls.strip('"')), method, sig))
	return out


def cpp_calls():
	calls = []
	for name in sorted(os.listdir(CPP)):
		if not name.endswith(".cpp"):
			continue
		with open(os.path.join(CPP, name)) as fh:
			calls.extend(parse_cpp_calls(fh.read(), name))
	return calls


def call_faults(calls, classes):
	"""[(what, sentence)] for every call that no Java method answers."""
	for where, cls, method, sig in calls:
		if cls not in classes:
			yield ("%s.%s" % (cls, method),
			        "src/%s calls into a class android/src does not define"
			        % where)
			continue
		offered = classes[cls].get(method)
		if not offered:
			yield ("%s.%s" % (cls, method),
			        "src/%s names a method that class does not declare static"
			        % where)
		elif sig not in offered:
			yield ("%s.%s" % (cls, method),
			        "src/%s asks for %s; the Java offers %s"
			        % (where, sig, " or ".join(offered)))


# --- the control ------------------------------------------------------------
#
# Deliberately broken samples, classified before any real file is read. A
# detector over a corpus that has always been clean cannot demonstrate that it
# is capable of speaking, and this one has been clean since it was written.

CONTROL_JAVA = """
package se.vibes.hydra;
class Sample {
	native void ok(String url, int id);
	native void extraArg(String url);
	native void swapped(String url, int id);
	native String wrongReturn(int id);
	native void gone(int id);
}
"""

CONTROL_CPP = """
JNIEXPORT void JNICALL Java_se_vibes_hydra_Sample_ok(JNIEnv *, jobject,
                                                      jstring url, jint id) {}
JNIEXPORT void JNICALL Java_se_vibes_hydra_Sample_extraArg(JNIEnv *, jobject,
                                                            jstring url, jint n) {}
JNIEXPORT void JNICALL Java_se_vibes_hydra_Sample_swapped(JNIEnv *, jobject,
                                                           jint id, jstring url) {}
JNIEXPORT void JNICALL Java_se_vibes_hydra_Sample_wrongReturn(JNIEnv *, jobject,
                                                               jint id) {}
JNIEXPORT void JNICALL Java_se_vibes_hydra_Sample_nobodyWants(JNIEnv *, jobject) {}
"""

# What the samples above must produce. Each names one way the checker claims
# to be able to fail; a checker that cannot produce these is not one whose
# silence on the real tree means anything.
# The other direction's samples: a class the Java defines, and calls out to it
# that are right, renamed, and wrongly typed.
CONTROL_JAVA_STATICS = """
package se.vibes.hydra;
import android.app.Activity;
class Sample {
	public static void fine(long id, String url) {}
	public static boolean open(Activity a, String url) { return true; }
}
"""

CONTROL_CPP_CALLS = """
static const char *k_cls = "se/vibes/hydra/Sample";
void a() { QJniObject::callStaticMethod<void>(k_cls, "fine",
                                               "(JLjava/lang/String;)V", 1); }
void b() { QJniObject::callStaticMethod<void>(k_cls, "renamed", "(J)V", 1); }
void c() { QJniObject::callStaticMethod<void>(k_cls, "fine", "(JI)V", 1); }
void d() { QJniObject::callStaticMethod<jboolean>("se/vibes/hydra/Nowhere",
                                                   "open", "()Z"); }
"""

CONTROL_CALLS_EXPECT = {
	"se/vibes/hydra/Sample.fine": "asks for",          # wrong descriptor
	"se/vibes/hydra/Sample.renamed": "does not declare",
	"se/vibes/hydra/Nowhere.open": "does not define",
}

CONTROL_EXPECT = {
	"Java_se_vibes_hydra_Sample_ok": None,
	"Java_se_vibes_hydra_Sample_extraArg": "argument",
	"Java_se_vibes_hydra_Sample_swapped": "argument",
	"Java_se_vibes_hydra_Sample_wrongReturn": "returns",
}


def self_test():
	"""[] when the checker classifies its own samples correctly."""
	complaints = []
	wanted = parse_java(CONTROL_JAVA, "Sample",
	                     lambda p, c, m: "control: %s.%s.%s" % (p, c, m))
	found = parse_cpp(CONTROL_CPP, "control")
	if len(wanted) != 5 or len(found) != 5:
		return ["the samples did not parse: %d java, %d cpp (want 5 and 5)"
		         % (len(wanted), len(found))]

	if "Java_se_vibes_hydra_Sample_gone" in found:
		complaints.append("a method with no C++ side was somehow found")
	if "Java_se_vibes_hydra_Sample_nobodyWants" in wanted:
		complaints.append("an orphan C++ symbol was somehow wanted")

	# The other direction, same discipline.
	statics = parse_java_statics(CONTROL_JAVA_STATICS, "se/vibes/hydra/Sample")
	calls = parse_cpp_calls(CONTROL_CPP_CALLS, "control.cpp")
	if len(calls) != 4:
		return ["the call samples did not parse: %d found, want 4" % len(calls)]
	out = dict(call_faults(calls, statics))
	for what, expect in CONTROL_CALLS_EXPECT.items():
		got = out.get(what)
		if got is None:
			complaints.append("%s is broken and was not reported" % what)
		elif expect not in got:
			complaints.append("%s was reported for the wrong reason: %s"
			                   % (what, got))
	# The one that is correct must be silent -- with three faults expected out
	# of four calls, a checker that complained about everything would satisfy
	# every line above.
	if len(out) != 3:
		complaints.append("%d call faults reported, want exactly 3" % len(out))

	faults = dict(signature_faults(wanted, found))
	for symbol, expect in CONTROL_EXPECT.items():
		got = faults.get(symbol)
		if expect is None and got is not None:
			complaints.append("%s is correct and was reported: %s"
			                   % (symbol.rsplit("_", 1)[-1], got))
		elif expect is not None and got is None:
			complaints.append("%s is broken and was not reported"
			                   % symbol.rsplit("_", 1)[-1])
		elif expect is not None and expect not in got:
			complaints.append("%s was reported for the wrong reason: %s"
			                   % (symbol.rsplit("_", 1)[-1], got))
	return complaints


def signature_faults(wanted, found):
	"""[(symbol, sentence)] for every pair whose types disagree.

	Only for symbols present on both sides -- one that is missing altogether is
	a louder fault reported above, and saying it twice would bury it.
	"""
	for symbol in sorted(wanted):
		if symbol not in found:
			continue
		where, want_args, want_ret = wanted[symbol]
		_, got_args, got_ret = found[symbol]
		if len(want_args) != len(got_args):
			yield symbol, ("%s declares %d argument(s); the C++ takes %d"
			                % (where, len(want_args), len(got_args)))
			continue
		for i, (w, g) in enumerate(zip(want_args, got_args)):
			if not compatible(w, g):
				yield symbol, ("%s argument %d is %s in Java and %s in the C++"
				                % (where, i + 1, w, g))
				break
		else:
			if not compatible(want_ret, got_ret):
				yield symbol, ("%s returns %s in Java and %s in the C++"
				                % (where, want_ret, got_ret))


def main():
	control = self_test()
	if control:
		print("jni-check: the control failed, so nothing below would mean "
		       "anything:")
		for line in control:
			print("  %s" % line)
		return 1

	wanted = java_natives()
	if wanted is None:
		return 1
	found = cpp_symbols()

	# **A check that inspected nothing passes as loudly as one that passed.**
	# If the Java tree moves or the regex stops matching, this would report
	# success over an empty set, which is the shape of failure the whole file
	# is about.
	if not wanted:
		print("jni-check: found no native methods to check at all.")
		print("  android/src is missing, or nothing in it declares one.")
		return 1

	classes = java_statics()
	calls = cpp_calls()
	outbound = sorted(call_faults(calls, classes))

	missing = sorted(s for s in wanted if s not in found)
	orphan = sorted(s for s in found if s not in wanted)
	crossed = sorted(signature_faults(wanted, found))

	for s in missing:
		print("MISSING  %s" % s)
		print("         declared by %s" % wanted[s][0])
		print("         JNI would throw UnsatisfiedLinkError on the first call")
	for s in orphan:
		print("ORPHAN   %s" % s)
		print("         defined in src/%s, and no Java native method wants it"
		      % found[s][0])
	for s, why in crossed:
		print("SIGNATURE %s" % s)
		print("          %s" % why)
		print("          JNI binds on the name alone, so this is called with "
		       "the arguments read as the C++ says")

	for what, why in outbound:
		print("CALL     %s" % what)
		print("         %s" % why)
		print("         this throws in Java the moment the feature is used, "
		       "on a device, with nothing at build time to say so")

	if missing or orphan or crossed or outbound:
		print()
		print("%d native method(s) and %d call(s) into Java checked, "
		       "%d unmatched"
		      % (len(wanted), len(calls),
		          len(missing) + len(orphan) + len(crossed) + len(outbound)))
		return 1

	# **The descriptors that could not be resolved are counted and said.** A
	# Java parameter whose class this tool cannot map produces a `?`, and a
	# call compared against a `?` has not been checked -- reporting it as
	# verified would be the vacuous pass this file's own control exists to
	# refuse.
	unresolved = sum(1 for methods in classes.values()
	                  for sigs in methods.values()
	                  for sig in sigs if "?" in sig)
	note = ""
	if unresolved:
		note = ", %d java signature(s) unresolved and not compared" % unresolved
	print("jni-check: %d native method(s) and %d call(s) into Java, "
	       "every one resolvable and matching%s"
	      % (len(wanted), len(calls), note))
	return 0


if __name__ == "__main__":
	sys.exit(main())
