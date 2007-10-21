/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2007 InspIRCd Development Team
 * See: http://www.inspircd.org/wiki/index.php/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include "wildcard.h"
#include "xline.h"
#include "socketengine.h"
#include "command_parse.h"

void FloodQuitUserHandler::Call(User* current)
{
	Server->Log(DEFAULT,"Excess flood from: %s@%s", current->ident, current->host);
	Server->SNO->WriteToSnoMask('f',"Excess flood from: %s%s%s@%s",
			current->registered == REG_ALL ? current->nick : "",
			current->registered == REG_ALL ? "!" : "", current->ident, current->host);
	User::QuitUser(Server, current, "Excess flood");
	if (current->registered != REG_ALL)
	{
		Server->XLines->add_zline(120, Server->Config->ServerName, "Flood from unregistered connection", current->GetIPString());
		Server->XLines->apply_lines(APPLY_ZLINES);
	}
}

void ProcessUserHandler::Call(User* cu)
{
	int result = EAGAIN;

	if (cu->GetFd() == FD_MAGIC_NUMBER)
		return;

	char* ReadBuffer = Server->GetReadBuffer();

	if (Server->Config->GetIOHook(cu->GetPort()))
	{
		int result2 = 0;
		int MOD_RESULT = 0;

		try
		{
			MOD_RESULT = Server->Config->GetIOHook(cu->GetPort())->OnRawSocketRead(cu->GetFd(),ReadBuffer,sizeof(ReadBuffer),result2);
		}
		catch (CoreException& modexcept)
		{
			Server->Log(DEBUG, "%s threw an exception: %s", modexcept.GetSource(), modexcept.GetReason());
		}

		if (MOD_RESULT < 0)
		{
			result = -EAGAIN;
		}
		else
		{
			result = result2;
		}
	}
	else
	{
		result = cu->ReadData(ReadBuffer, sizeof(ReadBuffer));
	}

	if ((result) && (result != -EAGAIN))
	{
		User *current;
		int currfd;
		int floodlines = 0;

		Server->stats->statsRecv += result;
		/*
		 * perform a check on the raw buffer as an array (not a string!) to remove
		 * character 0 which is illegal in the RFC - replace them with spaces.
		 */

		for (int checker = 0; checker < result; checker++)
		{
			if (ReadBuffer[checker] == 0)
				ReadBuffer[checker] = ' ';
		}

		if (result > 0)
			ReadBuffer[result] = '\0';

		current = cu;
		currfd = current->GetFd();

		// add the data to the users buffer
		if (result > 0)
		{
			if (!current->AddBuffer(ReadBuffer))
			{
				// AddBuffer returned false, theres too much data in the user's buffer and theyre up to no good.
				if (current->registered == REG_ALL)
				{
					// Make sure they arn't flooding long lines.
					if (Server->Time() > current->reset_due)
					{
						current->reset_due = Server->Time() + current->threshold;
						current->lines_in = 0;
					}

					current->lines_in++;

					if (current->flood && current->lines_in > current->flood)
						Server->FloodQuitUser(current);
					else
					{
						current->WriteServ("NOTICE %s :Your previous line was too long and was not delivered (Over %d chars) Please shorten it.", current->nick, MAXBUF-2);
						current->recvq.clear();
					}
				}
				else
					Server->FloodQuitUser(current);

				return;
			}

			// while there are complete lines to process...
			while (current->BufferIsReady())
			{
				if (Server->Time() > current->reset_due)
				{
					current->reset_due = Server->Time() + current->threshold;
					current->lines_in = 0;
				}

				if (++current->lines_in > current->flood && current->flood)
				{
					Server->FloodQuitUser(current);
					return;
				}

				if ((++floodlines > current->flood) && (current->flood != 0))
				{
					Server->FloodQuitUser(current);
					return;
				}

				// use GetBuffer to copy single lines into the sanitized string
				std::string single_line = current->GetBuffer();
				current->bytes_in += single_line.length();
				current->cmds_in++;
				if (single_line.length() > MAXBUF - 2)	/* MAXBUF is 514 to allow for neccessary line terminators */
					single_line.resize(MAXBUF - 2); /* So to trim to 512 here, we use MAXBUF - 2 */

				Server->Parser->ProcessBuffer(single_line, current);
			}

			return;
		}

		if ((result == -1) && (errno != EAGAIN) && (errno != EINTR))
		{
			User::QuitUser(Server, cu, errno ? strerror(errno) : "EOF from client");
			return;
		}
	}

	// result EAGAIN means nothing read
	else if ((result == EAGAIN) || (result == -EAGAIN))
	{
		/* do nothing */
	}
	else if (result == 0)
	{
		User::QuitUser(Server, cu, "Connection closed");
		return;
	}
}

/**
 * This function is called once a second from the mainloop.
 * It is intended to do background checking on all the user structs, e.g.
 * stuff like ping checks, registration timeouts, etc.
 */
void InspIRCd::DoBackgroundUserStuff()
{
	/*
	 * loop over all local users..
	 */
	for (std::vector<User*>::iterator count2 = local_users.begin(); count2 != local_users.end(); count2++)
	{
		User *curr = *count2;

		if ((curr->registered != REG_ALL) && (TIME > curr->timeout))
		{
			/*
			 * registration timeout -- didnt send USER/NICK/HOST
			 * in the time specified in their connection class.
			 */
			curr->muted = true;
			User::QuitUser(this, curr, "Registration timeout");
			continue;
		}

		/*
		 * `ready` means that the user has provided NICK/USER(/PASS), and all modules agree
		 * that the user is okay to proceed. The one thing we are then waiting for is DNS, which we do here...
		 */
		bool ready = ((curr->registered == REG_NICKUSER) && AllModulesReportReady(curr));

		if (ready)
		{
			if (!curr->dns_done)
			{
				/*
				 * DNS isn't done yet?
				 * Cool. Check for timeout.
				 */
				if (TIME > curr->signon)
				{
					/* FZZZZZZZZT, timeout! */
					curr->WriteServ("NOTICE Auth :*** Could not resolve your hostname: Request timed out; using your IP address (%s) instead.", curr->GetIPString());
					curr->dns_done = true;
					this->stats->statsDnsBad++;
					curr->FullConnect();
					continue;
				}
			}
			else
			{
				/* DNS passed, connect the user */
				curr->FullConnect();
				continue;
			}
		}

		// It's time to PING this user. Send them a ping.
		if ((TIME > curr->nping) && (curr->registered == REG_ALL))
		{
			// This user didn't answer the last ping, remove them
			if (!curr->lastping)
			{
				/* Everybody loves boobies. */
				time_t time = this->Time(false) - (curr->nping - curr->pingmax);
				char message[MAXBUF];
				snprintf(message, MAXBUF, "Ping timeout: %ld second%s", (long)time, time > 1 ? "s" : "");
				curr->muted = true;
				curr->lastping = 1;
				curr->nping = TIME+curr->pingmax;
				User::QuitUser(this, curr, message);
				continue;
			}
			curr->Write("PING :%s",this->Config->ServerName);
			curr->lastping = 0;
			curr->nping = TIME+curr->pingmax;
		}
	}
}

