#! /usr/bin/env python

from optparse import OptionParser

from xml.dom import minidom

import bcp

class ApplicationOptions(OptionParser, object):
    def __init__(self):
        from os import getenv

        super(ApplicationOptions, self).__init__()

        self.add_option(
            "--dsn",
            "--dbserver",
            dest="database_dsn",
            help="Database DSN to connect to",
            default="bdaws154",
        )

        self.add_option(
            "--db",
            "--dbname",
            dest="database_name",
            help="Database",
            default="pub",
        )

        self.add_option(
            "--dbuser",
            dest="database_user",
            help="Database Username",
            default="RENRE\\%s" % getenv("USER"),
        )

        self.add_option(
            "--dbpassword",
            dest="database_password",
            help="Database Password",
            default="",
        )

        self.add_option(
            "--dbowner",
            dest="database_owner",
            help="Database Owner",
            default="dbo",
        )

        self.add_option(
            "--dbtable",
            dest="database_table",
            help="Database table for bulk copy destination",
            default='my_test_table',
        )

myxml = minidom.parse("/tmp/fred.xml").toxml().encode('utf-16')
nullxml = minidom.parseString("<doc>None</doc>").toxml().encode('utf-16')

ROW_DATA = [
    ['me', 2, nullxml],
    ['you', 1, ""],
    ['them', 12.177, ""],
    ['us', 99.2, ""],
    ['who',None, myxml],
]

def main(argv):
    (options, args) = ApplicationOptions().parse_args(argv)

    bcp_connection = bcp.Connection(
        server = options.database_dsn,
        username = options.database_user,
        password = options.database_password,
        database = options.database_name,
        batchsize = 16, # 16 rows before bcp_batch is called, set to 0 to disable,
        textsize = 33554432,
    )

    bcp_connection.simplequery("set textsize 16777216")

    try:
        bcp_connection.simplequery(
            "drop table [%s].[%s].[%s]" % (
                options.database_name,
                options.database_owner,
                options.database_table,
            ),

            print_results=False
        )
    except (bcp.DblibError), e:
        print e

    bcp_connection.simplequery(
        "create table [%s].[%s].[%s](name varchar(256), age float null, xmldata xml null)" % (
            options.database_name,
            options.database_owner,
            options.database_table,
        ),

        print_results=False
    )

    bcp_connection.init(
        "[%s].[%s].[%s]" % (
            options.database_name,
            options.database_owner,
            options.database_table,
        )
    )

    for row in ROW_DATA:
        bcp_connection.send(row)

    bcp_connection.done()

    bcp_connection.simplequery(
        "select name, age from [%s].[%s].[%s]" % (
            options.database_name,
            options.database_owner,
            options.database_table,
        ),

        print_results=True
    )

    bcp_connection.disconnect()

if __name__ == "__main__":
    from sys import exit, argv, stderr

    try:
        main(argv[1:])
    except KeyboardInterrupt:
        print >> stderr, "Exiting due to <ctrl-c>"
        exit(0)


