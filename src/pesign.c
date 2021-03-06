/*
 * Copyright 2011-2012 Red Hat, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s): Peter Jones <pjones@redhat.com>
 */

#include <err.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <nss.h>

#include <popt.h>

#include <prerror.h>
#include <cert.h>
#include <pkcs7t.h>

#include "pesign.h"

#define NO_FLAGS		0x00
#define GENERATE_DIGEST		0x01
#define GENERATE_SIGNATURE	0x02
#define IMPORT_RAW_SIGNATURE	0x04
#define IMPORT_SIGNATURE	0x08
#define IMPORT_SATTRS		0x10
#define EXPORT_SATTRS		0x20
#define EXPORT_SIGNATURE	0x40
#define REMOVE_SIGNATURE	0x80
#define LIST_SIGNATURES		0x100
#define PRINT_DIGEST		0x200
#define EXPORT_PUBKEY		0x400
#define EXPORT_CERT		0x800
#define DAEMONIZE		0x1000
#define FLAG_LIST_END		0x2000

static struct {
	int flag;
	const char *name;
} flag_names[] = {
	{DAEMONIZE, "daemonize"},
	{GENERATE_DIGEST, "hash"},
	{GENERATE_SIGNATURE, "sign"},
	{IMPORT_RAW_SIGNATURE, "import-raw-sig"},
	{IMPORT_SIGNATURE, "import-sig"},
	{IMPORT_SATTRS, "import-sattrs" },
	{EXPORT_SATTRS, "export-sattrs" },
	{EXPORT_SIGNATURE, "export-sig"},
	{EXPORT_PUBKEY, "export-pubkey"},
	{EXPORT_CERT, "export-cert"},
	{REMOVE_SIGNATURE, "remove"},
	{LIST_SIGNATURES, "list"},
	{FLAG_LIST_END, NULL},
};

static void
print_flag_name(FILE *f, int flag)
{
	for (int i = 0; flag_names[i].flag != FLAG_LIST_END; i++) {
		if (flag_names[i].flag == flag)
			fprintf(f, "%s ", flag_names[i].name);
	}
}

static void
open_input(pesign_context *ctx)
{
	if (!ctx->infile) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	struct stat statbuf;
	ctx->infd = open(ctx->infile, O_RDONLY|O_CLOEXEC);
	stat(ctx->infile, &statbuf);
	ctx->outmode = statbuf.st_mode;

	if (ctx->infd < 0) {
		fprintf(stderr, "pesign: Error opening input: %m\n");
		exit(1);
	}

	Pe_Cmd cmd = ctx->infd == STDIN_FILENO ? PE_C_READ : PE_C_READ_MMAP;
	ctx->inpe = pe_begin(ctx->infd, cmd, NULL);
	if (!ctx->inpe) {
		fprintf(stderr, "pesign: could not load input file: %s\n",
			pe_errmsg(pe_errno()));
		exit(1);
	}

	int rc = parse_signatures(&ctx->cms_ctx->signatures,
				  &ctx->cms_ctx->num_signatures, ctx->inpe);
	if (rc < 0) {
		fprintf(stderr, "pesign: could not parse signature list in "
			"EFI binary\n");
		exit(1);
	}
}

static void
close_input(pesign_context *ctx)
{
	pe_end(ctx->inpe);
	ctx->inpe = NULL;

	close(ctx->infd);
	ctx->infd = -1;
}

static void
close_output(pesign_context *ctx)
{
	Pe_Cmd cmd = ctx->outfd == STDOUT_FILENO ? PE_C_RDWR : PE_C_RDWR_MMAP;

	finalize_signatures(ctx->cms_ctx->signatures,
				ctx->cms_ctx->num_signatures, ctx->outpe);
	pe_update(ctx->outpe, cmd);
	pe_end(ctx->outpe);
	ctx->outpe = NULL;

	close(ctx->outfd);
	ctx->outfd = -1;
}

static void
open_output(pesign_context *ctx)
{
	if (!ctx->outfile) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outfile, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force was "
				"not given.\n", ctx->outfile);
		exit(1);
	}

	ctx->outfd = open(ctx->outfile, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
			ctx->outmode);
	if (ctx->outfd < 0) {
		fprintf(stderr, "pesign: Error opening output: %m\n");
		exit(1);
	}

	size_t size;
	char *addr;

	addr = pe_rawfile(ctx->inpe, &size);

	ftruncate(ctx->outfd, size);
	lseek(ctx->outfd, 0, SEEK_SET);
	write(ctx->outfd, addr, size);

	Pe_Cmd cmd = ctx->outfd == STDOUT_FILENO ? PE_C_RDWR : PE_C_RDWR_MMAP;
	ctx->outpe = pe_begin(ctx->outfd, cmd, NULL);
	if (!ctx->outpe) {
		fprintf(stderr, "pesign: could not load output file: %s\n",
			pe_errmsg(pe_errno()));
		exit(1);
	}

	pe_clearcert(ctx->outpe);
}

static void
open_rawsig_input(pesign_context *ctx)
{
	if (!ctx->rawsig) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	ctx->rawsigfd = open(ctx->rawsig, O_RDONLY|O_CLOEXEC);
	if (ctx->rawsigfd < 0) {
		fprintf(stderr, "pesign: Error opening raw signature for input:"
				" %m\n");
		exit(1);
	}
}

static void
close_rawsig_input(pesign_context *ctx)
{
	close(ctx->rawsigfd);
	ctx->rawsigfd = -1;
}

static void
open_sattr_input(pesign_context *ctx)
{
	if (!ctx->insattrs) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	ctx->insattrsfd = open(ctx->insattrs, O_RDONLY|O_CLOEXEC);
	if (ctx->insattrsfd < 0) {
		fprintf(stderr, "pesign: Error opening signed attributes "
				"for input: %m\n");
		exit(1);
	}
}

static void
close_sattr_input(pesign_context *ctx)
{
	close(ctx->insattrsfd);
	ctx->insattrsfd = -1;
}

static void
open_sattr_output(pesign_context *ctx)
{
	if (!ctx->outsattrs) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outsattrs, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force "
				"was not given.\n", ctx->outsattrs);
		exit(1);
	}

	ctx->outsattrsfd = open(ctx->outsattrs,
			O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
			ctx->outmode);
	if (ctx->outsattrsfd < 0) {
		fprintf(stderr, "pesign: Error opening signed attributes "
				"for output: %m\n");
		exit(1);
	}
}

static void
close_sattr_output(pesign_context *ctx)
{
	close(ctx->outsattrsfd);
	ctx->outsattrsfd = -1;
}

static void
open_sig_input(pesign_context *ctx)
{
	if (!ctx->insig) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	ctx->insigfd = open(ctx->insig, O_RDONLY|O_CLOEXEC);
	if (ctx->insigfd < 0) {
		fprintf(stderr, "pesign: Error opening signature for input: "
				"%m\n");
		exit(1);
	}
}

static void
close_sig_input(pesign_context *ctx)
{
	close(ctx->insigfd);
	ctx->insigfd = -1;
}

static void
open_sig_output(pesign_context *ctx)
{
	if (!ctx->outsig) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outsig, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force "
				"was not given.\n", ctx->outsig);
		exit(1);
	}

	ctx->outsigfd = open(ctx->outsig, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
				ctx->outmode);
	if (ctx->outsigfd < 0) {
		fprintf(stderr, "pesign: Error opening signature for output: "
				"%m\n");
		exit(1);
	}
}

static void
close_sig_output(pesign_context *ctx)
{
	close(ctx->outsigfd);
	ctx->outsigfd = -1;
}

static void
open_pubkey_output(pesign_context *ctx)
{
	if (!ctx->outkey) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outkey, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force "
				"was not given.\n", ctx->outkey);
		exit(1);
	}

	ctx->outkeyfd = open(ctx->outkey, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
				ctx->outmode);
	if (ctx->outkeyfd < 0) {
		fprintf(stderr, "pesign: Error opening pubkey for output: "
				"%m\n");
		exit(1);
	}
}

static void
open_cert_output(pesign_context *ctx)
{
	if (!ctx->outcert) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (access(ctx->outcert, F_OK) == 0 && ctx->force == 0) {
		fprintf(stderr, "pesign: \"%s\" exists and --force "
				"was not given.\n", ctx->outcert);
		exit(1);
	}

	ctx->outcertfd = open(ctx->outcert, O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC,
				ctx->outmode);
	if (ctx->outcertfd < 0) {
		fprintf(stderr, "pesign: Error opening certificate for output: "
				"%m\n");
		exit(1);
	}
}

static void
check_inputs(pesign_context *ctx)
{
	if (!ctx->infile) {
		fprintf(stderr, "pesign: No input file specified.\n");
		exit(1);
	}

	if (!ctx->outfile) {
		fprintf(stderr, "pesign: No output file specified.\n");
		exit(1);
	}

	if (!strcmp(ctx->infile, ctx->outfile)) {
		fprintf(stderr, "pesign: in-place file editing "
				"is not yet supported\n");
		exit(1);
	}
}

static void
print_digest(pesign_context *pctx)
{
	if (!pctx)
		return;

	cms_context *ctx = pctx->cms_ctx;
	if (!ctx)
		return;

	printf("hash: ");
	int j = ctx->selected_digest;
	for (unsigned int i = 0; i < ctx->digests[j].pe_digest->len; i++)
		printf("%02x",
			(unsigned char)ctx->digests[j].pe_digest->data[i]);
	printf("\n");
}

int
main(int argc, char *argv[])
{
	int rc;

	pesign_context *ctxp;

	int list = 0;
	int remove = 0;
	int daemon = 0;
	int fork = 1;
	int padding = 0;
	int need_db = 0;

	char *digest_name = "sha256";
	char *tokenname = "NSS Certificate DB";
	char *origtoken = tokenname;
	char *certname = NULL;
	char *certdir = "/etc/pki/pesign";
	char *signum = NULL;

	rc = pesign_context_new(&ctxp);
	if (rc < 0) {
		fprintf(stderr, "Could not initialize context: %m\n");
		exit(1);
	}

	poptContext optCon;
	struct poptOption options[] = {
		{.argInfo = POPT_ARG_INTL_DOMAIN,
		 .arg = "pesign" },
		{.longName = "in",
		 .shortName = 'i',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &ctxp->infile,
		 .descrip = "specify input file",
		 .argDescrip = "<infile>"},
		{.longName = "out",
		 .shortName = 'o',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &ctxp->outfile,
		 .descrip = "specify output file",
		 .argDescrip = "<outfile>" },
		{.longName = "certficate",
		 .shortName = 'c',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &certname,
		 .descrip = "specify certificate nickname",
		 .argDescrip = "<certificate nickname>" },
		{.longName = "certdir",
		 .shortName = 'n',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT,
		 .arg = &certdir,
		 .descrip = "specify nss certificate database directory",
		 .argDescrip = "<certificate directory path>" },
		{.longName = "force",
		 .shortName = 'f',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &ctxp->force,
		 .val = 1,
		 .descrip = "force overwriting of output file" },
		{.longName = "sign",
		 .shortName = 's',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &ctxp->sign,
		 .val = 1,
		 .descrip = "create a new signature" },
		{.longName = "hash",
		 .shortName = 'h',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &ctxp->hash,
		 .val = 1,
		 .descrip = "hash binary" },
		{.longName = "digest_type",
		 .shortName = 'd',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT,
		 .arg = &digest_name,
		 .descrip = "digest type to use for pe hash" },
		{.longName = "import-signed-certificate",
		 .shortName = 'm',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &ctxp->insig,
		 .descrip = "<insig>" },
		{.longName = "export-signed-attributes",
		 .shortName = 'E',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &ctxp->outsattrs,
		 .descrip = "export signed attributes to file",
		 .argDescrip = "<signed_attributes_file>" },
		{.longName = "import-signed-attributes",
		 .shortName = 'I',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &ctxp->insattrs,
		 .descrip = "import signed attributes from file",
		 .argDescrip = "<signed_attributes_file>" },
		{.longName = "import-raw-signature",
		 .shortName = 'R',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &ctxp->rawsig,
		 .descrip = "import raw signature from file",
		 .argDescrip = "<inraw>" },
		{.longName = "signature-number",
		 .shortName = 'u',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &signum,
		 .descrip = "specify which signature to operate on","<sig-number>"},
		{.longName = "list-signatures",
		 .shortName = 'l',
		 .argInfo = POPT_ARG_VAL|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &list,
		 .val = 1,
		 .descrip = "list signatures" },
		{.longName = "nss-token",
		 .shortName = 't',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_SHOW_DEFAULT,
		 .arg = &tokenname,
		 .descrip = "NSS token holding signing key" },
		{.longName = "show-signature",
		 .shortName = 'S',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &list,
		 .val = 1,
		 .descrip = "show signature" },
		{.longName = "remove-signature",
		 .shortName = 'r',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &remove,
		 .val = 1,
		 .descrip = "remove signature" },
		{.longName = "export-signature",
		 .shortName = 'e',
		 .argInfo = POPT_ARG_STRING|POPT_ARGFLAG_DOC_HIDDEN,
		 .arg = &ctxp->outsig,
		 .descrip = "export signature to file",
		 .argDescrip = "<outsig>" },
		{.longName = "export-pubkey",
		 .shortName = 'K',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &ctxp->outkey,
		 .descrip = "export pubkey to file",
		 .argDescrip = "<outkey>" },
		{.longName = "export-cert",
		 .shortName = 'C',
		 .argInfo = POPT_ARG_STRING,
		 .arg = &ctxp->outcert,
		 .descrip = "export signing cert to file",
		 .argDescrip = "<outcert>" },
		{.longName = "ascii-armor",
		 .shortName = 'a',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &ctxp->ascii,
		 .val = 1,
		 .descrip = "use ascii armoring" },
		{.longName = "daemonize",
		 .shortName = 'D',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &daemon,
		 .val = 1,
		 .descrip = "run as a daemon process" },
		{.longName = "nofork",
		 .shortName = 'N',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &fork,
		 .descrip = "don't fork when daemonizing" },
		{.longName = "verbose",
		 .shortName = 'v',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &ctxp->verbose,
		 .val = 1,
		 .descrip = "be very verbose" },
		{.longName = "padding",
		 .shortName = 'P',
		 .argInfo = POPT_ARG_VAL,
		 .arg = &padding,
		 .val = 1,
		 .descrip = "pad data section" },
		POPT_AUTOALIAS
		POPT_AUTOHELP
		POPT_TABLEEND
	};

	optCon = poptGetContext("pesign", argc, (const char **)argv, options,0);

	rc = poptReadDefaultConfig(optCon, 0);
	if (rc < 0 && !(rc == POPT_ERROR_ERRNO && errno == ENOENT)) {
		fprintf(stderr, "pesign: poptReadDefaultConfig failed: %s\n",
		poptStrerror(rc));
		exit(1);
	}

	while ((rc = poptGetNextOpt(optCon)) > 0)
		;

	if (rc < -1) {
		fprintf(stderr, "pesign: Invalid argument: %s: %s\n",
			poptBadOption(optCon, 0), poptStrerror(rc));
		exit(1);
	}

	if (poptPeekArg(optCon)) {
		fprintf(stderr, "pesign: Invalid Argument: \"%s\"\n",
				poptPeekArg(optCon));
		exit(1);
	}

	poptFreeContext(optCon);

	if (signum) {
		errno = 0;
		ctxp->signum = strtol(signum, NULL, 0);
		if (errno != 0) {
			fprintf(stderr, "invalid signature number: %m\n");
			exit(1);
		}
	}

	int action = 0;
	if (daemon)
		action |= DAEMONIZE;

	if (ctxp->rawsig) {
		action |= IMPORT_RAW_SIGNATURE;
		need_db = 1;
	}

	if (ctxp->insattrs)
		action |= IMPORT_SATTRS;

	if (ctxp->outsattrs)
		action |= EXPORT_SATTRS;

	if (ctxp->insig)
		action |= IMPORT_SIGNATURE;

	if (ctxp->outkey) {
		action |= EXPORT_PUBKEY;
		need_db = 1;
	}

	if (ctxp->outcert) {
		action |= EXPORT_CERT;
		need_db = 1;
	}

	if (ctxp->outsig)
		action |= EXPORT_SIGNATURE;

	if (remove != 0)
		action |= REMOVE_SIGNATURE;

	if (list != 0)
		action |= LIST_SIGNATURES;

	if (ctxp->sign) {
		action |= GENERATE_SIGNATURE;
		if (!(action & EXPORT_SIGNATURE))
			action |= IMPORT_SIGNATURE;
		need_db = 1;
	}

	if (ctxp->hash)
		action |= GENERATE_DIGEST|PRINT_DIGEST;

	if (!daemon) {
		SECStatus status;
		if (need_db) {
			status = NSS_Init(certdir);
			if (status != SECSuccess) {
				char *globpattern = NULL;
				rc = asprintf(&globpattern, "%s/cert*.db",
					      certdir);
				if (rc > 0) {
					glob_t globbuf;
					memset(&globbuf, 0, sizeof(globbuf));
					rc = glob(globpattern, GLOB_ERR, NULL,
						  &globbuf);
					if (rc != 0) {
						err(1, "Could not open NSS database (\"%s\")",
						     PORT_ErrorToString(PORT_GetError()));
					}
				}
			}
		} else
			status = NSS_NoDB_Init(NULL);
		if (status != SECSuccess) {
			errx(1, "Could not initialize nss. NSS says \"%s\" errno says \"%m\"\n",
				PORT_ErrorToString(PORT_GetError()));
		}

		status = register_oids(ctxp->cms_ctx);
		if (status != SECSuccess) {
			fprintf(stderr, "Could not register OIDs\n");
			exit(1);
		}
	}

	rc = set_digest_parameters(ctxp->cms_ctx, digest_name);
	int is_help  = strcmp(digest_name, "help") ? 0 : 1;
	if (rc < 0) {
		if (!is_help) {
			fprintf(stderr, "Digest \"%s\" not found.\n",
				digest_name);
		}
		exit(!is_help);
	}

	ctxp->cms_ctx->tokenname = tokenname ?
		PORT_ArenaStrdup(ctxp->cms_ctx->arena, tokenname) : NULL;
	if (tokenname && !ctxp->cms_ctx->tokenname) {
		fprintf(stderr, "could not allocate token name: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		exit(1);
	}
	if (tokenname != origtoken)
		free(tokenname);

	ctxp->cms_ctx->certname = certname ?
		PORT_ArenaStrdup(ctxp->cms_ctx->arena, certname) : NULL;
	if (certname && !ctxp->cms_ctx->certname) {
		fprintf(stderr, "could not allocate certificate name: %s\n",
			PORT_ErrorToString(PORT_GetError()));
		exit(1);
	}
	if (certname)
		free(certname);


	if (ctxp->sign) {
		if (!ctxp->cms_ctx->certname) {
			fprintf(stderr, "pesign: signing requested but no "
				"certificate nickname provided\n");
			exit(1);
		}
	}

	ssize_t sigspace = 0;

	switch (action) {
		case NO_FLAGS:
			fprintf(stderr, "pesign: Nothing to do.\n");
			exit(0);
			break;
		/* in this case we have the actual binary signature and the
		 * signing cert, but not the pkcs7ish certificate that goes
		 * with it.
		 */
		case IMPORT_RAW_SIGNATURE|IMPORT_SATTRS:
			check_inputs(ctxp);
			rc = find_certificate(ctxp->cms_ctx, 0);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_rawsig_input(ctxp);
			open_sattr_input(ctxp);
			import_raw_signature(ctxp);
			close_sattr_input(ctxp);
			close_rawsig_input(ctxp);

			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			sigspace = calculate_signature_space(ctxp->cms_ctx,
								ctxp->outpe);
			allocate_signature_space(ctxp->outpe, sigspace);
			generate_signature(ctxp->cms_ctx);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_output(ctxp);
			break;
		case EXPORT_SATTRS:
			open_input(ctxp);
			open_sattr_output(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, 1);
			generate_sattr_blob(ctxp);
			close_sattr_output(ctxp);
			close_input(ctxp);
			break;
		/* add a signature from a file */
		case IMPORT_SIGNATURE:
			check_inputs(ctxp);
			if (ctxp->signum > ctxp->cms_ctx->num_signatures + 1) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			open_sig_input(ctxp);
			parse_signature(ctxp);
			sigspace = get_sigspace_extend_amount(ctxp->cms_ctx,
					ctxp->outpe, &ctxp->cms_ctx->newsig);
			allocate_signature_space(ctxp->outpe, sigspace);
			check_signature_space(ctxp);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_sig_input(ctxp);
			close_output(ctxp);
			break;
		case EXPORT_PUBKEY:
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_pubkey_output(ctxp);
			export_pubkey(ctxp);
			break;
		case EXPORT_CERT:
			rc = find_certificate(ctxp->cms_ctx, 0);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_cert_output(ctxp);
			export_cert(ctxp);
			break;
		/* find a signature in the binary and save it to a file */
		case EXPORT_SIGNATURE:
			open_input(ctxp);
			open_sig_output(ctxp);
			if (ctxp->signum > ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			if (ctxp->signum < 0)
				ctxp->signum = 0;
			if (ctxp->signum >= ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "No valid signature #%d.\n",
					ctxp->signum);
				exit(1);
			}
			memcpy(&ctxp->cms_ctx->newsig,
				ctxp->cms_ctx->signatures[ctxp->signum],
				sizeof (ctxp->cms_ctx->newsig));
			export_signature(ctxp->cms_ctx, ctxp->outsigfd, ctxp->ascii);
			close_input(ctxp);
			close_sig_output(ctxp);
			memset(&ctxp->cms_ctx->newsig, '\0',
				sizeof (ctxp->cms_ctx->newsig));
			break;
		/* remove a signature from the binary */
		case REMOVE_SIGNATURE:
			check_inputs(ctxp);
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			if (ctxp->signum < 0 ||
					ctxp->signum >=
					ctxp->cms_ctx->num_signatures) {
				fprintf(stderr, "Invalid signature number %d.  "
					"Must be between 0 and %d.\n",
					ctxp->signum,
					ctxp->cms_ctx->num_signatures - 1);
				exit(1);
			}
			remove_signature(ctxp);
			close_output(ctxp);
			break;
		/* list signatures in the binary */
		case LIST_SIGNATURES:
			open_input(ctxp);
			list_signatures(ctxp);
			break;
		case GENERATE_DIGEST|PRINT_DIGEST:
			open_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, padding);
			print_digest(ctxp);
			break;
		/* generate a signature and save it in a separate file */
		case EXPORT_SIGNATURE|GENERATE_SIGNATURE:
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			open_input(ctxp);
			open_sig_output(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->inpe, 1);
			generate_signature(ctxp->cms_ctx);
			export_signature(ctxp->cms_ctx, ctxp->outsigfd, ctxp->ascii);
			break;
		/* generate a signature and embed it in the binary */
		case IMPORT_SIGNATURE|GENERATE_SIGNATURE:
			check_inputs(ctxp);
			rc = find_certificate(ctxp->cms_ctx, 1);
			if (rc < 0) {
				fprintf(stderr, "pesign: Could not find "
					"certificate %s\n",
					ctxp->cms_ctx->certname);
				exit(1);
			}
			if (ctxp->signum > ctxp->cms_ctx->num_signatures + 1) {
				fprintf(stderr, "Invalid signature number.\n");
				exit(1);
			}
			open_input(ctxp);
			open_output(ctxp);
			close_input(ctxp);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			sigspace = calculate_signature_space(ctxp->cms_ctx,
							     ctxp->outpe);
			allocate_signature_space(ctxp->outpe, sigspace);
			generate_digest(ctxp->cms_ctx, ctxp->outpe, 1);
			generate_signature(ctxp->cms_ctx);
			insert_signature(ctxp->cms_ctx, ctxp->signum);
			close_output(ctxp);
			break;
		case DAEMONIZE:
			rc = daemonize(ctxp->cms_ctx, certdir, fork);
			break;
		default:
			fprintf(stderr, "Incompatible flags (0x%08x): ", action);
			for (int i = 1; i < FLAG_LIST_END; i <<= 1) {
				if (action & i)
					print_flag_name(stderr, i);
			}
			fprintf(stderr, "\n");
			exit(1);
	}
	pesign_context_free(ctxp);

	if (!daemon) {
		SECStatus status = NSS_Shutdown();
		if (status != SECSuccess) {
			fprintf(stderr, "could not shut down NSS: %s",
				PORT_ErrorToString(PORT_GetError()));
			exit(1);
		}
	}

	return (rc < 0);
}
