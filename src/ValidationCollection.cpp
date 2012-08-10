
#include "ValidationCollection.h"

#include "Application.h"
#include "LedgerTiming.h"
#include "Log.h"

bool ValidationCollection::addValidation(SerializedValidation::pointer val)
{
	NewcoinAddress signer = val->getSignerPublic();
	bool isCurrent = false;
	if (theApp->getUNL().nodeInUNL(signer))
	{
		val->setTrusted();
		uint32 now = theApp->getOPs().getCloseTimeNC();
		uint32 valClose = val->getCloseTime();
		if ((now > (valClose - 4)) && (now < (valClose + LEDGER_MAX_INTERVAL)))
			isCurrent = true;
		else
			Log(lsWARNING) << "Received stale validation now=" << now << ", close=" << valClose;
	}
	else Log(lsINFO) << "Node " << signer.humanNodePublic() << " not in UNL";

	uint256 hash = val->getLedgerHash();
	uint160 node = signer.getNodeID();

	{
		boost::mutex::scoped_lock sl(mValidationLock);
		if (!mValidations[hash].insert(std::make_pair(node, val)).second)
			return false;
		if (isCurrent)
		{
			boost::unordered_map<uint160, ValidationPair>::iterator it = mCurrentValidations.find(node);
			if ((it == mCurrentValidations.end()) || (!it->second.newest) ||
				(val->getCloseTime() > it->second.newest->getCloseTime()))
			{
				if (it != mCurrentValidations.end())
				{
					if  (it->second.oldest)
					{
						mStaleValidations.push_back(it->second.oldest);
						condWrite();
					}
					it->second.oldest = it->second.newest;
					it->second.newest = val;
				}
				else
					mCurrentValidations.insert(std::make_pair(node, ValidationPair(val)));
			}
		}
	}

	Log(lsINFO) << "Val for " << hash.GetHex() << " from " << signer.humanNodePublic()
		<< " added " << (val->isTrusted() ? "trusted" : "UNtrusted");
	return isCurrent;
}

ValidationSet ValidationCollection::getValidations(const uint256& ledger)
{
	ValidationSet ret;
	{
		boost::mutex::scoped_lock sl(mValidationLock);
		boost::unordered_map<uint256, ValidationSet>::iterator it = mValidations.find(ledger);
		if (it != mValidations.end())
			ret = it->second;
	}
	return ret;
}

void ValidationCollection::getValidationCount(const uint256& ledger, bool currentOnly, int& trusted, int &untrusted)
{
	trusted = untrusted = 0;
	boost::mutex::scoped_lock sl(mValidationLock);
	boost::unordered_map<uint256, ValidationSet>::iterator it = mValidations.find(ledger);
	uint32 now = theApp->getOPs().getCloseTimeNC();
	if (it != mValidations.end())
	{
		for (ValidationSet::iterator vit = it->second.begin(), end = it->second.end(); vit != end; ++vit)
		{
			bool isTrusted = vit->second->isTrusted();
			if (isTrusted && currentOnly)
			{
				uint32 closeTime = vit->second->getCloseTime();
				if ((now < closeTime) || (now > (closeTime + 2 * LEDGER_MAX_INTERVAL)))
					isTrusted = false;
			}
			if (isTrusted)
				++trusted;
			else
				++untrusted;
		}
	}
}

int ValidationCollection::getTrustedValidationCount(const uint256& ledger)
{
	int trusted = 0;
	boost::mutex::scoped_lock sl(mValidationLock);
	for (boost::unordered_map<uint256, ValidationSet>::iterator it = mValidations.find(ledger),
		end = mValidations.end(); it != end; ++it)
	{
		for (ValidationSet::iterator vit = it->second.begin(), end = it->second.end(); vit != end; ++vit)
		{
			if (vit->second->isTrusted())
				++trusted;
		}
	}
	return trusted;
}

int ValidationCollection::getCurrentValidationCount(uint32 afterTime)
{
	int count = 0;
	boost::mutex::scoped_lock sl(mValidationLock);
	for (boost::unordered_map<uint160, ValidationPair>::iterator it = mCurrentValidations.begin(),
		end = mCurrentValidations.end(); it != end; ++it)
	{
		if (it->second.newest->isTrusted() && (it->second.newest->getCloseTime() > afterTime))
			++count;
	}
	return count;
}

boost::unordered_map<uint256, int> ValidationCollection::getCurrentValidations()
{
    uint32 now = theApp->getOPs().getCloseTimeNC();
	boost::unordered_map<uint256, int> ret;

	{
		boost::mutex::scoped_lock sl(mValidationLock);
		boost::unordered_map<uint160, ValidationPair>::iterator it = mCurrentValidations.begin();
		bool anyNew = false;
		while (it != mCurrentValidations.end())
		{
			ValidationPair& pair = it->second;

			if (pair.oldest && (now > (pair.oldest->getCloseTime() + LEDGER_MAX_INTERVAL)))
			{
				mStaleValidations.push_back(pair.oldest);
				pair.oldest = SerializedValidation::pointer();
				anyNew = true;
			}
			if (pair.newest && (now > (pair.newest->getCloseTime() + LEDGER_MAX_INTERVAL)))
			{
				mStaleValidations.push_back(pair.newest);
				pair.newest = SerializedValidation::pointer();
				anyNew = true;
			}
			if (!pair.newest && !pair.oldest)
				it = mCurrentValidations.erase(it);
			else
			{
				if (pair.oldest)
				{
					Log(lsTRACE) << "OLD " << pair.oldest->getLedgerHash().GetHex() << " " <<
						boost::lexical_cast<std::string>(pair.oldest->getCloseTime());
					++ret[pair.oldest->getLedgerHash()];
				}
				if (pair.newest)
				{
					Log(lsTRACE) << "NEW " << pair.newest->getLedgerHash().GetHex() << " " <<
						boost::lexical_cast<std::string>(pair.newest->getCloseTime());
					++ret[pair.newest->getLedgerHash()];
				}
				++it;
			}
		}
		if (anyNew)
			condWrite();
	}

	return ret;
}

bool ValidationCollection::isDeadLedger(const uint256& ledger)
{
	for (std::list<uint256>::iterator it = mDeadLedgers.begin(), end = mDeadLedgers.end(); it != end; ++it)
		if (*it == ledger)
			return true;
	return false;
}

void ValidationCollection::addDeadLedger(const uint256& ledger)
{
	if (isDeadLedger(ledger))
		return;

	mDeadLedgers.push_back(ledger);
	if (mDeadLedgers.size() >= 128)
		mDeadLedgers.pop_front();
}

void ValidationCollection::flush()
{
		boost::mutex::scoped_lock sl(mValidationLock);
		boost::unordered_map<uint160, ValidationPair>::iterator it = mCurrentValidations.begin();
		bool anyNew = false;
		while (it != mCurrentValidations.end())
		{
			if (it->second.oldest)
				mStaleValidations.push_back(it->second.oldest);
			if (it->second.newest)
				mStaleValidations.push_back(it->second.newest);
			++it;
			anyNew = true;
		}
		mCurrentValidations.clear();
		if (anyNew)
			condWrite();
		while (mWriting)
		{
			sl.unlock();
			boost::this_thread::sleep(boost::posix_time::milliseconds(100));
			sl.lock();
		}
}

void ValidationCollection::condWrite()
{
	if (mWriting)
		return;
	mWriting = true;
	boost::thread thread(boost::bind(&ValidationCollection::doWrite, this));
	thread.detach();
}

void ValidationCollection::doWrite()
{
	static boost::format insVal("INSERT INTO LedgerValidations "
		"(LedgerHash,NodePubKey,Flags,CloseTime,Signature) VALUES "
		"('%s','%s','%u','%u',%s);");

	boost::mutex::scoped_lock sl(mValidationLock);
	assert(mWriting);
	while (!mStaleValidations.empty())
	{
		std::vector<SerializedValidation::pointer> vector;
		mStaleValidations.swap(vector);
		sl.unlock();

		{
			ScopedLock dbl(theApp->getLedgerDB()->getDBLock());
			Database *db = theApp->getLedgerDB()->getDB();
			db->executeSQL("BEGIN TRANSACTION;");
			for (std::vector<SerializedValidation::pointer>::iterator it = vector.begin(); it != vector.end(); ++it)
				db->executeSQL(boost::str(insVal % (*it)->getLedgerHash().GetHex()
					% (*it)->getSignerPublic().humanNodePublic() % (*it)->getFlags() % (*it)->getCloseTime()
					% db->escape(strCopy((*it)->getSignature()))));
			db->executeSQL("END TRANSACTION;");
		}

		sl.lock();
	}
	mWriting = false;
}
