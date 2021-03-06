/*
   Copyright (C) Cfengine AS

   This file is part of Cfengine 3 - written and maintained by Cfengine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of Cfengine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include "generic_agent.h"
#include "mon.h"

#include "env_context.h"
#include "env_monitor.h"
#include "conversion.h"
#include "vars.h"
#include "logging_old.h"
#include "logging.h"
#include "signals.h"
#include "scope.h"
#include "sysinfo.h"

#ifdef HAVE_NOVA
#include "cf.nova.h"
#endif

typedef enum
{
    MONITOR_CONTROL_FORGET_RATE,
    MONITOR_CONTROL_MONITOR_FACILITY,
    MONITOR_CONTROL_HISTOGRAMS,
    MONITOR_CONTROL_TCP_DUMP,
    MONITOR_CONTROL_NONE
} MonitorControl;

static void ThisAgentInit(EvalContext *ctx);
static GenericAgentConfig *CheckOpts(int argc, char **argv);
static void KeepPromises(EvalContext *ctx, Policy *policy);

/*****************************************************************************/
/* Globals                                                                   */
/*****************************************************************************/

extern int NO_FORK;

extern const ConstraintSyntax CFM_CONTROLBODY[];

/*******************************************************************/
/* Command line options                                            */
/*******************************************************************/

static const char *ID = "The monitoring agent is a machine-learning, sampling\n"
    "daemon which learns the normal state of the current\n"
    "host and classifies new observations in terms of the\n"
    "patterns formed by previous ones. The data are made\n"
    "available to and read by cf-agent for classification\n" "of responses to anomalous states.";

static const struct option OPTIONS[14] =
{
    {"help", no_argument, 0, 'h'},
    {"debug", no_argument, 0, 'd'},
    {"verbose", no_argument, 0, 'v'},
    {"dry-run", no_argument, 0, 'n'},
    {"version", no_argument, 0, 'V'},
    {"no-lock", no_argument, 0, 'K'},
    {"file", required_argument, 0, 'f'},
    {"inform", no_argument, 0, 'I'},
    {"diagnostic", no_argument, 0, 'x'},
    {"no-fork", no_argument, 0, 'F'},
    {"histograms", no_argument, 0, 'H'},
    {"tcpdump", no_argument, 0, 'T'},
    {NULL, 0, 0, '\0'}
};

static const char *HINTS[14] =
{
    "Print the help message",
    "Enable debugging output",
    "Output verbose information about the behaviour of the agent",
    "All talk and no action mode - make no changes, only inform of promises not kept",
    "Output the version of the software",
    "Ignore system lock",
    "Specify an alternative input file than the default",
    "Print basic information about changes made to the system, i.e. promises repaired",
    "Activate internal diagnostics (developers only)",
    "Run process in foreground, not as a daemon",
    "Ignored for backward compatibility",
    "Interface with tcpdump if available to collect data about network",
    NULL
};

/*****************************************************************************/

int main(int argc, char *argv[])
{
    EvalContext *ctx = EvalContextNew();

    GenericAgentConfig *config = CheckOpts(argc, argv);
    GenericAgentConfigApply(ctx, config);

    GenericAgentDiscoverContext(ctx, config);
    Policy *policy = GenericAgentLoadPolicy(ctx, config);

    CheckForPolicyHub(ctx);

    ThisAgentInit(ctx);
    KeepPromises(ctx, policy);

    MonitorStartServer(ctx, policy);

    GenericAgentConfigDestroy(config);
    EvalContextDestroy(ctx);
    return 0;
}

/*******************************************************************/

static GenericAgentConfig *CheckOpts(int argc, char **argv)
{
    extern char *optarg;
    int optindex = 0;
    int c;
    GenericAgentConfig *config = GenericAgentConfigNewDefault(AGENT_TYPE_MONITOR);

    while ((c = getopt_long(argc, argv, "dvnIf:VSxHTKMFh", OPTIONS, &optindex)) != EOF)
    {
        switch ((char) c)
        {
        case 'f':
            GenericAgentConfigSetInputFile(config, GetWorkDir(), optarg);
            MINUSF = true;
            break;

        case 'd':
            config->debug_mode = true;
            NO_FORK = true;
            break;

        case 'K':
            IGNORELOCK = true;
            break;

        case 'I':
            INFORM = true;
            break;

        case 'v':
            VERBOSE = true;
            NO_FORK = true;
            break;

        case 'F':
            NO_FORK = true;
            break;

        case 'H':              /* Keep accepting this option for compatibility -- no longer used */
            break;

        case 'T':
            MonNetworkSnifferEnable(true);
            break;

        case 'V':
            PrintVersion();
            exit(0);

        case 'h':
            Syntax("cf-monitord", OPTIONS, HINTS, ID, true);
            exit(0);

        case 'M':
            ManPage("cf-monitord - cfengine's monitoring agent", OPTIONS, HINTS, ID);
            exit(0);

        case 'x':
            CfOut(OUTPUT_LEVEL_ERROR, "", "Self-diagnostic functionality is retired.");
            exit(0);

        default:
            Syntax("cf-monitord", OPTIONS, HINTS, ID, true);
            exit(1);
        }
    }

    if (!GenericAgentConfigParseArguments(config, argc - optind, argv + optind))
    {
        Log(LOG_LEVEL_ERR, "Too many arguments");
        exit(EXIT_FAILURE);
    }

    return config;
}

/*****************************************************************************/

static void KeepPromises(EvalContext *ctx, Policy *policy)
{
    Rval retval;

    Seq *constraints = ControlBodyConstraints(policy, AGENT_TYPE_MONITOR);
    if (constraints)
    {
        for (size_t i = 0; i < SeqLength(constraints); i++)
        {
            Constraint *cp = SeqAt(constraints, i);

            if (!IsDefinedClass(ctx, cp->classes, NULL))
            {
                continue;
            }

            if (!EvalContextVariableGet(ctx, (VarRef) { NULL, "control_monitor", cp->lval }, &retval, NULL))
            {
                CfOut(OUTPUT_LEVEL_ERROR, "", "Unknown lval %s in monitor control body", cp->lval);
                continue;
            }

            if (strcmp(cp->lval, CFM_CONTROLBODY[MONITOR_CONTROL_HISTOGRAMS].lval) == 0)
            {
                /* Keep accepting this option for backward compatibility. */
            }

            if (strcmp(cp->lval, CFM_CONTROLBODY[MONITOR_CONTROL_TCP_DUMP].lval) == 0)
            {
                MonNetworkSnifferEnable(BooleanFromString(retval.item));
            }

            if (strcmp(cp->lval, CFM_CONTROLBODY[MONITOR_CONTROL_FORGET_RATE].lval) == 0)
            {
                sscanf(retval.item, "%lf", &FORGETRATE);
                CfDebug("forget rate = %f\n", FORGETRATE);
            }
        }
    }
}

/*****************************************************************************/
/* Level 1                                                                   */
/*****************************************************************************/

static void ThisAgentInit(EvalContext *ctx)
{
    umask(077);
    sprintf(VPREFIX, "cf-monitord");

    SetReferenceTime(ctx, false);
    SetStartTime();

    signal(SIGINT, HandleSignalsForDaemon);
    signal(SIGTERM, HandleSignalsForDaemon);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, HandleSignalsForDaemon);
    signal(SIGUSR2, HandleSignalsForDaemon);

    FORGETRATE = 0.6;

    MonitorInitialize();
}
