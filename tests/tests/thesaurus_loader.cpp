#include <main.h>

#include "../../src/options.h"
#include "../../src/thesaurus.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
class MainPublications {
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<agi::dispatch::Thunk> pending;
	bool closing = false;

	public:
	void Post(agi::dispatch::Thunk thunk) {
		{
			std::scoped_lock lock(mutex);
			if (!closing) {
				pending.emplace_back(std::move(thunk));
				changed.notify_all();
				return;
			}
		}
		thunk();
	}

	bool WaitFor(std::size_t count) {
		std::unique_lock lock(mutex);
		return changed.wait_for(lock, std::chrono::seconds(5), [&] { return pending.size() >= count; }) && pending.size() == count;
	}

	std::size_t Size() {
		std::scoped_lock lock(mutex);
		return pending.size();
	}

	void Run(bool newest = false) {
		agi::dispatch::Thunk thunk;
		{
			std::scoped_lock lock(mutex);
			if (pending.empty())
				throw std::logic_error("No thesaurus publication is pending");
			if (newest) {
				thunk = std::move(pending.back());
				pending.pop_back();
			}
			else {
				thunk = std::move(pending.front());
				pending.pop_front();
			}
		}
		thunk();
	}

	std::deque<agi::dispatch::Thunk> Close() {
		std::scoped_lock lock(mutex);
		closing = true;
		return std::move(pending);
	}
};

class ThesaurusLoaderTest : public ::testing::Test {
	agi::Options options{"", R"({"Path":{"Dictionary":"?user/dictionaries"},"Tool":{"Thesaurus":{"Language":""}}})", agi::Options::FLUSH_SKIP};
	agi::Path paths;
	agi::Options *previous_options = config::opt;
	agi::Path *previous_path = config::path;
	agi::fs::path root;
	bool dispatch_initialized = false;

	void WriteDictionary(std::string const& language, std::string const& body) {
		std::ofstream index(root / "dictionaries" / ("th_" + language + ".idx"), std::ios::binary);
		index.exceptions(std::ios::badbit | std::ios::failbit);
		index << "UTF-8\n1\norbit|6\n";
		index.close();
		std::ofstream data(root / "dictionaries" / ("th_" + language + ".dat"), std::ios::binary);
		data.exceptions(std::ios::badbit | std::ios::failbit);
		data << "UTF-8\n"
			 << body;
		data.close();
	}

	protected:
	std::shared_ptr<MainPublications> publications = std::make_shared<MainPublications>();
	std::unique_ptr<Thesaurus> thesaurus;

	void SetUp() override {
		root = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-thesaurus-loader-%%%%%%%%");
		agi::fs::CreateDirectory(root / "dictionaries");
		agi::fs::CreateDirectory(root / "data" / "dictionaries");
		WriteDictionary("en_US", "orbit|2\n(noun)|orbit|course|flight path\n(verb)|orbit|circle|travel around\n");
		WriteDictionary("en_GB", "orbit|1\n(noun)|orbit|trajectory\n");
		paths.SetToken("?user", root);
		paths.SetToken("?data", root / "data");
		config::opt = &options;
		config::path = &paths;
		auto const main_thread = std::this_thread::get_id();
		agi::dispatch::Init([queue = publications](agi::dispatch::Thunk thunk) { queue->Post(std::move(thunk)); }, [main_thread] { return std::this_thread::get_id() == main_thread; }, [queue = publications] {
			std::size_t count = 0;
			while (queue->Size()) {
				queue->Run();
				++count;
			}
			return count; });
		dispatch_initialized = true;
	}

	void TearDown() override {
		thesaurus.reset();
		for (auto& publication : publications->Close())
			EXPECT_NO_THROW(publication());
		if (dispatch_initialized)
			agi::dispatch::Shutdown();
		config::opt = previous_options;
		config::path = previous_path;
		if (!root.empty())
			std::filesystem::remove_all(root);
	}

	void StartLoad(std::string const& language) {
		OPT_SET("Tool/Thesaurus/Language")->SetString(language);
		thesaurus = std::make_unique<Thesaurus>();
	}
};

TEST_F(ThesaurusLoaderTest, valid_load_publishes_grouped_entries) {
	StartLoad("en_US");
	ASSERT_TRUE(publications->WaitFor(1));
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	publications->Run();
	EXPECT_EQ((std::vector<Thesaurus::Entry>{{"(noun) orbit", {"course", "flight path"}}, {"(verb) orbit", {"circle", "travel around"}}}), thesaurus->Lookup("ORBIT"));
	EXPECT_EQ(0u, publications->Size());
}

TEST_F(ThesaurusLoaderTest, pending_load_does_not_publish_after_disabling) {
	StartLoad("en_US");
	ASSERT_TRUE(publications->WaitFor(1));
	OPT_SET("Tool/Thesaurus/Language")->SetString("");
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	publications->Run();
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	EXPECT_EQ(0u, publications->Size());
}

TEST_F(ThesaurusLoaderTest, pending_load_does_not_publish_after_missing_language) {
	StartLoad("en_US");
	ASSERT_TRUE(publications->WaitFor(1));
	OPT_SET("Tool/Thesaurus/Language")->SetString("zz_ZZ");
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	publications->Run();
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	EXPECT_EQ(0u, publications->Size());
}

TEST_F(ThesaurusLoaderTest, new_language_rejects_older_pending_load) {
	StartLoad("en_US");
	ASSERT_TRUE(publications->WaitFor(1));
	OPT_SET("Tool/Thesaurus/Language")->SetString("en_GB");
	ASSERT_TRUE(publications->WaitFor(2));
	publications->Run(true);
	EXPECT_EQ((std::vector<Thesaurus::Entry>{{"(noun) orbit", {"trajectory"}}}), thesaurus->Lookup("orbit"));
	publications->Run();
	EXPECT_EQ((std::vector<Thesaurus::Entry>{{"(noun) orbit", {"trajectory"}}}), thesaurus->Lookup("orbit"));
	EXPECT_EQ(0u, publications->Size());
}

TEST_F(ThesaurusLoaderTest, destroyed_owner_discards_pending_publication) {
	StartLoad("en_US");
	ASSERT_TRUE(publications->WaitFor(1));
	auto lifetime = thesaurus->GetAsyncUiLifetime();
	thesaurus.reset();
	EXPECT_TRUE(lifetime.expired());
	StartLoad("en_GB");
	ASSERT_TRUE(publications->WaitFor(2));
	publications->Run();
	EXPECT_TRUE(thesaurus->Lookup("orbit").empty());
	publications->Run();
	EXPECT_EQ((std::vector<Thesaurus::Entry>{{"(noun) orbit", {"trajectory"}}}), thesaurus->Lookup("orbit"));
	EXPECT_EQ(0u, publications->Size());
}
}
