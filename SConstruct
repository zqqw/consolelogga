# SConstruct

env = Environment(tools = ["default", "gettext", "textfile"])
from config_cache import USRPREFIX, LOGROTATEDIR, MANDIR, CONSOLELOGGA_CC, CONSOLELOGGA_USE_GETTEXT, STRIP_CONSOLELOGGA
# set default compile flags for gcc
env.Append(CCFLAGS = ["-Wall", "-Wextra", "-O2"])
# set user defined compiler if specified
if (CONSOLELOGGA_CC != "DEFAULT"):
	env['CC'] = CONSOLELOGGA_CC
#env['CC'] = 'gcc'
AddOption('--verbose',
	dest="verbose",
	default=False,
	action="store_true",
	help="Provide verbose output")
AddOption('--install',
	dest='install',
	action='store_true',
	help='Install the software',
	default=False)
AddOption('--uninstall',
	dest='uninstall',
	action='store_true',
	help='Uninstall the software',
	default=False)

# activate extra output with scons --verbose=true
if env.GetOption('verbose'):
	Progress('Evaluating $TARGET\n')

# compile consolelogga
csl = env.Program(["src/consolelogga.c"])

# strip consolelogga binary to reduce size unless otherwise instructed
if (STRIP_CONSOLELOGGA == 1):
	env.Command("src/consolelogga_tmp", "src/consolelogga", "strip src/consolelogga")

# update / create multi language support
env['POAUTOINIT'] = 1
SConscript('src/po/SConscript', exports = 'env')

if env.GetOption('uninstall'):
	env.SetOption('clean', 1)
if (env.GetOption('install')) or (env.GetOption('uninstall')):
	Default(env.Install(USRPREFIX + '/bin', csl))
	Default(env.Install([LOGROTATEDIR], ["src/scripts/consolelogga"]))
	Default(env.Install([MANDIR], ["src/man/consolelogga.8"]))
	if (CONSOLELOGGA_USE_GETTEXT == 1):
		Default(env.InstallAs([USRPREFIX + "/share/locale/cs/LC_MESSAGES/consolelogga.mo"], ["src/po/cs.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/de/LC_MESSAGES/consolelogga.mo"], ["src/po/de.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/en/LC_MESSAGES/consolelogga.mo"], ["src/po/en.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/es/LC_MESSAGES/consolelogga.mo"], ["src/po/es.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/et/LC_MESSAGES/consolelogga.mo"], ["src/po/et.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/fr/LC_MESSAGES/consolelogga.mo"], ["src/po/fr.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/hu/LC_MESSAGES/consolelogga.mo"], ["src/po/hu.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/it/LC_MESSAGES/consolelogga.mo"], ["src/po/it.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/nl/LC_MESSAGES/consolelogga.mo"], ["src/po/nl.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/pl/LC_MESSAGES/consolelogga.mo"], ["src/po/pl.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/pt/LC_MESSAGES/consolelogga.mo"], ["src/po/pt.mo"]))
		Default(env.InstallAs([USRPREFIX + "/share/locale/ru/LC_MESSAGES/consolelogga.mo"], ["src/po/ru.mo"]))
