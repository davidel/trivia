#!/usr/bin/perl -w
#
# send-serie.pl by Davide Libenzi (sends a set of quilt patch files)
# Copyright (C) 2007  Davide Libenzi
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#
# Davide Libenzi <davidel@xmailserver.org>
#
#
#
# I prefer to embed mail metadata inside the quilt patches headers,
# so that I can select per-patch recipients and subject. This script
# allows me to send patches to a remote SMTP server, or append them
# into a mailbox file. The patch mail metadata is a serie of RFC822-like
# headers, terminated by an empty line:
#
#   From: Davide Libenzi <davidel@xmailserver.org>
#   To: Linux Kernel Mailing List <linux-kernel@vger.kernel.org>
#   Cc: Andrew Morton <akpm@linux-foundation.org>,
#       Linus Torvalds <torvalds@linux-foundation.org>
#   Subject: anonymous inode source ...
#
# The send-serie.pl parses the mail metadata, figures out the target
# recipients, prepare the RFC822 mail content, and either deliver it
# to a remote SMTP server, or prints it to STDOUT in mailbox format.
# Since the send-serie.pl script uses STDIN/STDOUT handles to talk
# SMTP, something that links it to the remote server connection is
# needed. One way to use it to connect to a remote SMTPS server, is
# by using  stunnel  like:
#
#   $ stunnel -c -r HOST:465 -l send-serie.pl -- send-serie.pl ...
#
# Where HOST is the remote SMTP servr hostname.
# For normal SMTP connections, the  sxncat  tool can be use instead:
#
#   http://www.xmailserver.org/miscy.html#sxncat
#
# To be used like:
#
#   $ sxncat -s HOST -p 25 send-serie.pl ...
#
# Since when connecting to remote server, the STDERR handle in bound
# to the connection, the  -l  option can be used to specify a log file.
# This can be used together with the  -D  debug option, to print all
# the handshakes that happen between the send-serie.pl script and the
# remote server. The  -b  option selects the mailbox-mode, that
# does not connect to any remote server, but simply emit data in
# mailbox format.
# When in SMTP-mode, the send-serie.pl script can perform a LOGIN
# SMTP authentication to the remote server. The  -u  option allows
# to specify a USERNAME, while the  -p  option allows to specify
# a PASSWORD.
# Patches can be specified by name (if you supply the patch home
# directory with the  -d  option), or by full path.
# The  -s  option allows to specify a pre-subject that will be
# inserted between the [patch N/M] string and the real subject
# (the one specified with teh Subject: mail metadata):
#
#   [patch N/M] PRE - REAL
#
# The  -H  option allows you to add arbitrary headers into the RFC822
# header list (the  -H  option can be multiple).
# The patches specified in the send-serie.pl command lines are sent
# in the order they appear in the command line.
#

use strict;
use Net::Domain qw(hostname hostfqdn hostdomain);
use POSIX qw(strftime);
use MIME::Base64;
use IO::Handle;
use Socket;


my $debug = 0;
my $pdir = ".";
my $mbox = 0;
my $chfn;
my ($user, $passwd);
my @hdrs;
my $presubject;
my $patchbase = 1;
my @patches;
my $lfile = "send-serie.log";
my $LOGFILE = *STDERR;


sub err_out {
    my ($code, $msg) = @_;
    my @strs;

    @strs = split(/\r?\n/, $msg);
    foreach my $s (@strs) {
	print $LOGFILE "ERR: $s\n";
    }
    if ($code > 0) {
	exit($code);
    }
}

sub log_out {
    my ($msg) = @_;
    my @strs;

    @strs = split(/\r?\n/, $msg);
    foreach my $s (@strs) {
	print $LOGFILE "LOG: $s\n";
    }
}

sub make_date {
    return strftime ("%a, %d %b %Y %T %z", localtime);
}

sub addr_extract {
    my ($lst, $aa) = @_;
    my $n = 0;

    while ($lst =~ /^[^\<]*\<([^\>]+)\>(.*)$/s) {
	push(@$aa, $1);
	$lst = $2;
	$n++;
    }

    return $n;
}

sub smtp_resp {
    my ($cls) = @_;
    my $code = -1;
    my $ln;
    my $resp = "";

    while (<STDIN>) {
	log_out("< $_") if ($debug);
	$resp .= $_;
	if (!/^([0-9]{3})([ \-])(.*)$/) {
	    err_out(2, "bad SMTP response: $resp\n");
	}
	$code = scalar($1);
	if ($2 eq " ") {
	    last;
	}
    }
    if ($code < $cls || $code >= $cls + 100) {
	err_out(2, "unexpected SMTP response: $resp\n");
    }
    return $resp;
}

sub smtp_chat {
    my ($cmd, $cls) = @_;
    my @strs;

    log_out("> $cmd") if ($debug);
    @strs = split(/\r?\n/, $cmd);
    foreach my $s (@strs) {
	print $s . "\r\n";
    }
    return smtp_resp($cls);
}

sub auth_login {
    my ($usr, $pwd) = @_;

    smtp_chat("AUTH LOGIN\r\n", 300);
    smtp_chat(encode_base64($usr, "\r\n"), 300);
    smtp_chat(encode_base64($pwd, "\r\n"), 200);
}

sub merge_headers {
    my ($dh, $sh) = @_;
    my %mrghow = (
	to => ", ",
	cc => ", ",
	bcc => ", ",
	subject => " ",
	DATA => "",
    );

    foreach my $k (keys %$sh) {
	if (defined($$dh{$k}) && defined($mrghow{$k})) {
	    $$dh{$k} .= $mrghow{$k} . $$sh{$k};
	} else {
	    $$dh{$k} = $$sh{$k};
	}
    }
}

sub parse_patch {
    my ($fn, $hp) = @_;
    my $inhdr = 1;
    my $line = 0;
    my $nam;

    if (!open(PFIL, $fn)) {
	err_out(0, "unable to open patch file: $fn\n");
	return -1;
    }
    %$hp = ();
    while (<PFIL>) {
	my $ln = $_;

	$line++;
	if ($inhdr) {
	    if ($ln !~ /[^ \t\r\n]+/) {
		$inhdr = 0;
		$$hp{DATA} = "";
		next;
	    }
	    if ($ln =~ /^([^:]+):[ \t]*([^\r\n]*)[\r\n]+$/) {
		$nam = lc($1);
		$$hp{$nam} = $2;
	    } elsif ($ln =~ /^([ \t]+)([^\r\n]*)[\r\n]+$/) {
		if (!defined($nam)) {
		    err_out(0, "wrong patch file format: $fn (line $line)\n");
		    close(PFIL);
		    return -1;
		}
		$$hp{$nam} .= "\n" . $1 . $2;
	    } else {
		err_out(0, "wrong patch file format: $fn (line $line)\n");
		close(PFIL);
		return -1;
	    }
	} else {
	    if ($mbox) {
		if ($ln =~ /^From[ \t]+/i) {
		    $ln = "> $ln";
		}
	    } else {
		if ($ln =~ /^\.[\r\n]+$/) {
		    $ln = ".$ln";
		}
	    }
	    $$hp{DATA} .= $ln;
	}
    }
    close(PFIL);
    return 0;
}

sub usage {
    print $LOGFILE "use: $0 [-d PATCHDIR] [-b] [-s PRESUBJ] [-u USER] [-p PASSWD]\n";
    print $LOGFILE "\t[-l LOGFILE] [-D] [-H HDR] [-c CHFILE] [-i PATCH] PATCH ...\n";
    exit(1);
}

for (my $i = 0; $i <= $#ARGV; $i++) {
    if ($ARGV[$i] eq "-d") {
	if (++$i <= $#ARGV) {
	    $pdir = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-s") {
	if (++$i <= $#ARGV) {
	    $presubject = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-u") {
	if (++$i <= $#ARGV) {
	    $user = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-l") {
	if (++$i <= $#ARGV) {
	    $lfile = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-p") {
	if (++$i <= $#ARGV) {
	    $passwd = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-c") {
	if (++$i <= $#ARGV) {
	    $chfn = $ARGV[$i];
	}
    } elsif ($ARGV[$i] eq "-H") {
	if (++$i <= $#ARGV) {
	    push(@hdrs, $ARGV[$i]);
	}
    } elsif ($ARGV[$i] eq "-b") {
	$mbox++;
    } elsif ($ARGV[$i] eq "-D") {
	$debug++;
    } elsif ($ARGV[$i] eq "-h") {
	usage();
    } elsif ($ARGV[$i] eq "-i") {
	if ($patchbase > 0 && ++$i <= $#ARGV) {
	    $patchbase = 0;
	    push(@patches, $ARGV[$i]);
	}
    } else {
	push(@patches, $ARGV[$i]);
    }
}

if (!$mbox) {
    if (!open(LFILE, ">$lfile")) {
	exit(3);
    }
    autoflush LFILE 1;
    $LOGFILE = *LFILE;
    autoflush STDOUT 1;
    autoflush STDERR 1;
    smtp_resp(200);
    smtp_chat("EHLO " . hostfqdn() . "\r\n", 200);
    if (defined($user) && defined($passwd)) {
	auth_login($user, $passwd);
    }
}

if (!(-d $pdir)) {
    err_out(1, "invalid directory: $pdir\n");
}
if (defined($chfn) && !(-f $chfn)) {
    err_out(1, "common headers file not found: $chfn\n");
}

if ($#patches < 0) {
    err_out(1, "no patches has been specified\n");
}

my @parsed;
my %cmnh = ();

if (defined($chfn)) {
    my $fn = $chfn;

    if ($fn !~ /\//) {
	$fn = "$pdir/$fn";
    }
    if (parse_patch($fn, \%cmnh) < 0) {
	exit(1);
    }
}

foreach my $fn (@patches) {
    my %hp;

    if ($fn !~ /\//) {
	$fn = "$pdir/$fn";
    }
    if (!(-f $fn)) {
	err_out(1, "patch file not found: $fn\n");
    }
    if (parse_patch($fn, \%hp) < 0) {
	exit(1);
    }
    merge_headers(\%hp, \%cmnh);
    push(@parsed, \%hp);
}

my $date;
my $npatches = $#parsed;
my $n = $patchbase;

$npatches += $patchbase;
$date = make_date();
foreach my $p (@parsed) {
    my $subject;
    my @from;
    my @to;
    my @cc;
    my @bcc;
    my $mdata = "";

    if (!defined($$p{subject})) {
	err_out(1, "missing Subject from patch" . $patches[$n] . "\n");
    }
    if (defined($presubject)) {
	$$p{subject} = $presubject . " - " . $$p{subject};
    }
    $subject = "[patch " . $n . "/" . $npatches . "] " . $$p{subject};
    if (!defined($$p{from})) {
	err_out(1, "missing From from patch" . $patches[$n] . "\n");
    }
    if (addr_extract($$p{from}, \@from) <= 0) {
	err_out(1, "missing address from: " . $$p{from} . "\n");
    }
    $mdata .= "From: " . $$p{from} . "\n";
    if (!defined($$p{to})) {
	err_out(1, "missing To from patch" . $patches[$n] . "\n");
    }
    if (addr_extract($$p{to}, \@to) <= 0) {
	err_out(1, "missing address from: " . $$p{to} . "\n");
    }
    $mdata .= "To: " . $$p{to} . "\n";
    if (defined($$p{cc})) {
	if (addr_extract($$p{cc}, \@cc) <= 0) {
	    err_out(1, "missing address from: " . $$p{cc} . "\n");
	}
	$mdata .= "Cc: " . $$p{cc} . "\n";
    }
    if (defined($$p{bcc})) {
	if (addr_extract($$p{bcc}, \@bcc) <= 0) {
	    err_out(1, "missing address from: " . $$p{bcc} . "\n");
	}
    }
    $mdata .= "Date: " . $date . "\n";
    $mdata .= "Subject: " . $subject . "\n";
    $mdata .= "MIME-Version: 1.0\n";
    $mdata .= "Content-Type: TEXT/PLAIN; charset=US-ASCII\n";
    $mdata .= "Message-ID: <send-serie." . $from[0] . "." . $$ . "." . time() . "." . $n . ">\n";
    foreach my $h (@hdrs) {
	$mdata .= $h . "\n";
    }
    $mdata .= "\n";
    $mdata .= $$p{DATA};

    log_out("sending patch: $subject\n") if ($debug);
    if ($mbox) {
	print "From " . $from[0] . " " . scalar(localtime) . "\n";
	print $mdata;
	print "\n";
    } else {
	smtp_chat("MAIL FROM:<" . $from[0] . ">\r\n", 200);
	foreach my $rcpt (@to) {
	    smtp_chat("RCPT TO:<" . $rcpt . ">\r\n", 200);
	}
	foreach my $rcpt (@cc) {
	    smtp_chat("RCPT TO:<" . $rcpt . ">\r\n", 200);
	}
	foreach my $rcpt (@bcc) {
	    smtp_chat("RCPT TO:<" . $rcpt . ">\r\n", 200);
	}
	smtp_chat("DATA\r\n", 300);
	smtp_chat($mdata . "\r\n.\r\n", 200);
    }
    $n++;
}

if (!$mbox) {
    smtp_chat("QUIT\r\n", 200);
}

