#!python3
from parts import *

#enable smart linking
SetOptionDefault("LINKFLAGS", ['-Wl,--copy-dt-needed-entries', '-Wl,--as-needed'])
SetOptionDefault("CXXFLAGS", ['-std=c++17'])
SetOptionDefault("INSTALL_ROOT", "#")
#SetOptionDefault("INSTALL_LIB",  "#lib")
#SetOptionDefault("INSTALL_BIN",  "#bin")
#SetOptionDefault("INSTALL_INCLUDE",  "#include")
#SetOptionDefault("INSTALL_PKGCONFIG", "#lib/pkgconfig")

# control shim for trafficserver
AddOption("--with-trafficserver",
          dest='with_trafficserver',
          nargs=1,
          type='string',
          action='store',
          metavar='DIR',
          default=None,
          help='Optional path to custom build of trafficserver')

AddOption("--with-ssl",
          dest='with_ssl',
          nargs=1,
          type='string',
          action='store',
          metavar='DIR',
          default=None,
          help='Optional path to custom build of OpenSSL'
         )


# the depends
Part("swoc++/swoc++.part"
        ,vcs_type=VcsGit(server="github.com", repository="SolidWallOfCode/libswoc", tag="dev-1-0-11")
        )
Part("#lib/libyaml-cpp.part", vcs_type=VcsGit(server="github.com", repository="jbeder/yaml-cpp.git", protocol="https", tag="yaml-cpp-0.6.3"))

# this is just a shim part. it passes info based on stuff being installed on the box
# it should have a better check for the real version of trafficserver being used
ts_path = GetOption("with_trafficserver")
Part("#lib/trafficserver.part", PKG_PREFIX=ts_path, PKG_VERSION="10.0.0")

path = GetOption("with_ssl")
Part("#lib/openssl.part", CUSTOM_PATH=path)
Part("#lib/libyaml-cpp.part")

# The main build.
Part("plugin/txn_box.part", package_group='txn_box')
