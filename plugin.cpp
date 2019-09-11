/*
 * Fledge "Python 2.7" filter plugin.
 *
 * Copyright (c) 2018 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <utils.h>
#include <version.h>

#include "python27.h"

// Relative path to FLEDGE_DATA
#define PYTHON_FILTERS_PATH "/scripts"
#define FILTER_NAME "python27"
#define PYTHON_SCRIPT_METHOD_PREFIX "_script_"
#define PYTHON_SCRIPT_FILENAME_EXTENSION ".py"
#define SCRIPT_CONFIG_ITEM_NAME "script"

/**
 * The Python 2.7 script module to load is set in
 * 'script' config item and it doesn't need the trailing .py
 *
 * Example:
 * if filename is 'readings_filter.py', just set 'readings_filter'
 * via Fledge configuration managewr
 *
 * Note:
 * Python 2.7 filter code needs two methods.
 *
 * One is the filtering method to call which must have
 * the same as the script name: it can not be changed.
 * The second one is the configuration entry point
 * method 'set_filter_config': it can not be changed
 *
 * Example: readings_filter.py
 *
 * expected two methods:
 * - set_filter_config(configuration) // Input is a string
 *   It sets the configuration internally as dict
 *
 * - readings_filter(readings) // Input is a dict
 *   It returns a dict with filtered input data
 */

// Filter default configuration
#define DEFAULT_CONFIG "{\"plugin\" : { \"description\" : \"Python 2.7 filter plugin\", " \
                       		"\"type\" : \"string\", " \
				"\"readonly\": \"true\", " \
				"\"default\" : \"" FILTER_NAME "\" }, " \
			 "\"enable\": {\"description\": \"A switch that can be used to enable or disable execution of " \
					 "the Python 2.7 filter.\", " \
				"\"type\": \"boolean\", " \
				"\"displayName\": \"Enabled\", " \
				"\"default\": \"false\" }, " \
			"\"config\" : {\"description\" : \"Python 2.7 filter configuration.\", " \
				"\"type\" : \"JSON\", " \
				"\"displayName\" : \"Configuration\", " \
				"\"order\": \"2\", " \
				"\"default\" : \"{}\"}, " \
			"\"script\" : {\"description\" : \"Python 2.7 module to load.\", " \
				"\"type\": \"script\", " \
				"\"displayName\" : \"Python Script\", " \
				"\"order\": \"1\", " \
				"\"default\": \"""\"} }"

bool pythonInitialised = false;

using namespace std;

/**
 * The Filter plugin interface
 */
extern "C" {
/**
 * The plugin information structure
 */
static PLUGIN_INFORMATION info = {
        FILTER_NAME,              // Name
        VERSION,                  // Version
        0,                        // Flags
        PLUGIN_TYPE_FILTER,       // Type
        "1.0.0",                  // Interface version
	DEFAULT_CONFIG	          // Default plugin configuration
};

typedef struct
{
	Python27Filter	*handle;
	std::string	configCatName;
} FILTER_INFO;

/**
 * Return the information about this plugin
 */
PLUGIN_INFORMATION *plugin_info()
{
	return &info;
}

/**
 * Initialise the plugin, called to get the plugin handle and setup the
 * output handle that will be passed to the output stream. The output stream
 * is merely a function pointer that is called with the output handle and
 * the new set of readings generated by the plugin.
 *     (*output)(outHandle, readings);
 * Note that the plugin may not call the output stream if the result of
 * the filtering is that no readings are to be sent onwards in the chain.
 * This allows the plugin to discard data or to buffer it for aggregation
 * with data that follows in subsequent calls
 *
 * @param config	The configuration category for the filter
 * @param outHandle	A handle that will be passed to the output stream
 * @param output	The output stream (function pointer) to which data is passed
 * @return		An opaque handle that is used in all subsequent calls to the plugin
 */
PLUGIN_HANDLE plugin_init(ConfigCategory* config,
			  OUTPUT_HANDLE *outHandle,
			  OUTPUT_STREAM output)
{
	FILTER_INFO *info = new FILTER_INFO;
	info->handle = new Python27Filter(FILTER_NAME,
						*config,
						outHandle,
						output);
	info->configCatName = config->getName();
	Python27Filter *pyFilter = info->handle;

	// Embedded Python 2.7 program name
	Py_SetProgramName((char *)config->getName().c_str());

	// Embedded Python 2.7 initialisation
	if (!Py_IsInitialized())
	{
		Py_Initialize();
		PyEval_InitThreads(); // Initialize and acquire the global interpreter lock (GIL)
		PyThreadState* save = PyEval_SaveThread(); // release GIL
		pythonInitialised = true;
	}

	// Pass Fledge Data dir
	pyFilter->setFiltersPath(getDataDir());

	PyGILState_STATE state = PyGILState_Ensure(); // acquire GIL

	// Set Python path for embedded Python 2.7
	// Get current sys.path. borrowed reference
	PyObject* sysPath = PySys_GetObject((char *)string("path").c_str());
	// Add Fledge python filters path
	PyObject* pPath = PyString_FromString((char *)pyFilter->getFiltersPath().c_str());
	PyList_Insert(sysPath, 0, pPath);
	// Remove temp object
	Py_CLEAR(pPath);

	// Check first we have a Python script to load
	if (!pyFilter->setScriptName())
	{
		// Force disable
		pyFilter->disableFilter();

		PyGILState_Release(state);

		// Return filter handle
		return (PLUGIN_HANDLE)info;
	}

	// Configure filter
	if (!pyFilter->configure())
	{
		if (pythonInitialised)
		{
			pythonInitialised = false;
		}

		PyGILState_Release(state);
		// This will abort the filter pipeline set up
		return NULL;
	}
	else
	{
		PyGILState_Release(state);
		// Return filter handle
		return (PLUGIN_HANDLE)info;
	}
}

/**
 * Ingest a set of readings into the plugin for processing
 *
 * NOTE: in case of any error, the input readings will be passed
 * onwards (untouched)
 *
 * @param handle	The plugin handle returned from plugin_init
 * @param readingSet	The readings to process
 */
void plugin_ingest(PLUGIN_HANDLE *handle,
		   READINGSET *readingSet)
{
	FILTER_INFO *info = (FILTER_INFO *) handle;
	Python27Filter *filter = info->handle;

	// Protect against reconfiguration
	filter->lock();
	bool enabled = filter->isEnabled();
	filter->unlock();

	if (!filter->isEnabled())
	{
		// Current filter is not active: just pass the readings set
		filter->m_func(filter->m_data, readingSet);
		return;
	}

        // Get all the readings in the readingset
	const vector<Reading *>& readings = ((ReadingSet *)readingSet)->getAllReadings();
	for (vector<Reading *>::const_iterator elem = readings.begin();
						      elem != readings.end();
						      ++elem)
	{
		AssetTracker::getAssetTracker()->addAssetTrackingTuple(info->configCatName, (*elem)->getAssetName(), string("Filter"));
	}
	
	/**
	 * 1 - create a Python object (list of dicts) from input data
	 * 2 - pass Python object to Python filter method
	 * 3 - Transform results from fealter into new ReadingSet
	 * 4 - Remove old data and pass new data set onwards
	 */

	PyGILState_STATE state = PyGILState_Ensure();

	// - 1 - Create Python list of dicts as input to the filter
	PyObject* readingsList = filter->createReadingsList(readings);

	// Check for errors
	if (!readingsList)
	{
		// Errors while creating Python 2.7 filter input object
		Logger::getLogger()->error("Filter '%s' (%s), script '%s', "
					   "create filter data error, action: %s",
					   FILTER_NAME,
					   filter->getConfig().getName().c_str(),
					   filter->m_pythonScript.c_str(),
					  "pass unfiltered data onwards");

		// Pass data set to next filter and return
		filter->m_func(filter->m_data, readingSet);
		PyGILState_Release(state);
		return;
	}

	// - 2 - Call Python method passing an object
	PyObject* pReturn = PyObject_CallFunction(filter->m_pFunc,
						  (char *)string("O").c_str(),
						  readingsList);

	// Free filter input data
	Py_CLEAR(readingsList);

	ReadingSet* finalData = NULL;

	// - 3 - Handle filter returned data
	if (!pReturn)
	{
		// Errors while getting result object
		Logger::getLogger()->error("Filter '%s' (%s), script '%s', "
					   "filter error, action: %s",
					   FILTER_NAME,
					   filter->getConfig().getName().c_str(),
					   filter->m_pythonScript.c_str(),
					   "pass unfiltered data onwards");

		// Errors while getting result object
		filter->logErrorMessage();

		// Filter did nothing: just pass input data
		finalData = (ReadingSet *)readingSet;
	}
	else
	{
		// Get new set of readings from Python filter
		vector<Reading *>* newReadings = filter->getFilteredReadings(pReturn);
		if (newReadings)
		{
			// Filter success
			// - Delete input data as we have a new set
			delete (ReadingSet *)readingSet;
			readingSet = NULL;

			// - Set new readings with filtered/modified data
			finalData = new ReadingSet(newReadings);

			const vector<Reading *>& readings2 = finalData->getAllReadings();
			for (vector<Reading *>::const_iterator elem = readings2.begin();
								      elem != readings2.end();
								      ++elem)
			{
				AssetTracker::getAssetTracker()->addAssetTrackingTuple(info->configCatName, (*elem)->getAssetName(), string("Filter"));
			}

			// - Remove newReadings pointer
			delete newReadings;
		}
		else
		{
			// Filtered data error: use current reading set
			finalData = (ReadingSet *)readingSet;
		}

		// Remove pReturn object
		Py_CLEAR(pReturn);
	}

	PyGILState_Release(state);

	// - 4 - Pass (new or old) data set to next filter
	filter->m_func(filter->m_data, finalData);
}

/**
 * Call the shutdown method in the plugin
 */
void plugin_shutdown(PLUGIN_HANDLE *handle)
{
	FILTER_INFO *info = (FILTER_INFO *) handle;
	Python27Filter* filter = info->handle;

	PyGILState_STATE state = PyGILState_Ensure();

	// Decrement pModule reference count
	Py_CLEAR(filter->m_pModule);
	// Decrement pFunc reference count
	Py_CLEAR(filter->m_pFunc);

	// Cleanup Python 2.7
	if (pythonInitialised)
	{
		pythonInitialised = false;
		Py_Finalize();
	}

	// Free plugin handle object
	delete filter;

	delete info;
}

/**
 * Apply filter plugin reconfiguration
 *
 * @param    handle	The plugin handle returned from plugin_init
 * @param    newConfig	The new configuration to apply.
 */
void plugin_reconfigure(PLUGIN_HANDLE *handle, const string& newConfig)
{
	FILTER_INFO *info = (FILTER_INFO *) handle;
	Python27Filter* filter = info->handle;

	PyGILState_STATE state = PyGILState_Ensure();
	filter->reconfigure(newConfig);

	PyGILState_Release(state);
}

// End of extern "C"
};
