#!/usr/bin/ruby
require 'mkmf'

Config::CONFIG['CC']  = 'g++'
Config::CONFIG['CPP'] = 'g++'

$CFLAGS  = '-fPIC -Os'

def apt_install_hint pkg
  "sudo apt-get install #{pkg}"
end

def library_installed? name, hint
  if have_library(name)
    true
  else
    $stderr.puts <<-ERROR

      Unable to find required library: #{name}.
      On debian systems, it can be installed as,

      #{hint}

      You may have to add the following ppa to your sources,

      sudo add-apt-repository ppa:deepfryed

      to install dbic++-dev and associated drivers dbic++-mysql or dbic++-pg

    ERROR
    false
  end
end

def assert_dbicpp_version ver
  passed  = false
  header  = '/usr/include/dbic++.h'
  message = "Swift needs dbic++ >= #{ver}. Please update your dbic++ installation."

  if File.exists?(header) && match = File.read(header).match(/DBI_VERSION\s+(.*?)\n/mi)
    rmajor, rminor, rbuild = ver.strip.split(/\./).map(&:to_i)
    imajor, iminor, ibuild = match.captures.first.strip.split(/\./).map(&:to_i)
    passed = (imajor >  rmajor) ||
             (imajor == rmajor && iminor >  rminor) ||
             (imajor == rmajor && iminor == rminor && ibuild >= rbuild)
  else
    message = "Cannot find #{header} or version number. You need to install dbic++ >= #{ver}"
    passed  = false
  end

  raise message unless passed
end

exit 1 unless library_installed? 'pcrecpp', apt_install_hint('libpcre3-dev')
exit 1 unless library_installed? 'uuid',    apt_install_hint('uuid-dev')
exit 1 unless library_installed? 'dbic++',  apt_install_hint('dbic++-dev')

assert_dbicpp_version '0.4.0'

create_makefile 'swift'
