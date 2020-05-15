/*
 * Fledge "Python 2.7" filter plugin.
 *
 * Copyright (c) 2018 Dianomic Systems
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string>
#include <iostream>

#include "python27.h"

// Relative path to FLEDGE_DATA
#define PYTHON_SCRIPT_METHOD_PREFIX "_script_"
#define PYTHON_SCRIPT_FILENAME_EXTENSION ".py"
#define SCRIPT_CONFIG_ITEM_NAME "script"

// Filter configuration method
#define DEFAULT_FILTER_CONFIG_METHOD "set_filter_config"

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

using namespace std;

/**
 * Create a Python 2.7 object (list of dicts)
 * to be passed to Python 2.7 loaded filter
 *
 * @param readings	The input readings
 * @return		PyObject pointer (list of dicts)
 *			or NULL in case of errors
 */
PyObject* Python27Filter::createReadingsList(const vector<Reading *>& readings)
{
	// TODO add checks to all PyList_XYZ methods
	PyObject* readingsList = PyList_New(0);

	// Iterate the input readings
	for (vector<Reading *>::const_iterator elem = readings.begin();
                                                      elem != readings.end();
                                                      ++elem)
	{
		// Create an object (dict) with 'asset_code' and 'readings' key
		PyObject* readingObject = PyDict_New();

		// Create object (dict) for reading Datapoints:
		// this will be added as vale for key 'readings'
		PyObject* newDataPoints = PyDict_New();

		// Get all datapoints
		std::vector<Datapoint *>& dataPoints = (*elem)->getReadingData();
		for (auto it = dataPoints.begin(); it != dataPoints.end(); ++it)
		{
			PyObject* value;
			DatapointValue::dataTagType dataType = (*it)->getData().getType();

			if (dataType == DatapointValue::dataTagType::T_INTEGER)
			{
				value = PyInt_FromLong((*it)->getData().toInt());
			}
			else if (dataType == DatapointValue::dataTagType::T_FLOAT)
			{
				value = PyFloat_FromDouble((*it)->getData().toDouble());
			}
			else
			{
				value = PyString_FromString((*it)->getData().toString().c_str());
			}

			// Add Datapoint: key and value
			PyDict_SetItemString(newDataPoints,
					     (*it)->getName().c_str(),
					     value);
			Py_CLEAR(value);
		}

		// Add reading datapoints
		PyDict_SetItemString(readingObject, "reading", newDataPoints);

		// Add reading asset name
		PyObject* assetVal = PyString_FromString((*elem)->getAssetName().c_str());
		PyDict_SetItemString(readingObject, "asset_code", assetVal);

		/**
		 * Save id, timestamp and user_timestamp
		 */
		// Add reading id
		PyObject* readingId = PyLong_FromUnsignedLong((*elem)->getId());
		PyDict_SetItemString(readingObject, "id", readingId);

		// Add reading timestamp
		PyObject* readingTs = PyLong_FromUnsignedLong((*elem)->getTimestamp());
		PyDict_SetItemString(readingObject, "ts", readingTs);

		// Add reading user timestamp
		PyObject* readingUserTs = PyLong_FromUnsignedLong((*elem)->getUserTimestamp());
		PyDict_SetItemString(readingObject, "user_ts", readingUserTs);

		// Add new object to the list
		PyList_Append(readingsList, readingObject);

		Py_CLEAR(newDataPoints);
		Py_CLEAR(assetVal);
		Py_CLEAR(readingId);
		Py_CLEAR(readingTs);
		Py_CLEAR(readingUserTs);
		Py_CLEAR(readingObject);
	}

	// Return pointer of new allocated list
	return readingsList;
}

/**
 * Get the vector of filtered readings from Python 2.7 script
 *
 * @param filteredData	Python 2.7 Object (list of dicts)
 * @return		Pointer to a new allocated vector<Reading *>
 *			or NULL in case of errors
 * Note:
 * new readings have:
 * - new timestamps
 */
vector<Reading *>* Python27Filter::getFilteredReadings(PyObject* filteredData)
{
	// Create result set
	vector<Reading *>* newReadings = new vector<Reading *>();

	// Iterate filtered data in the list
	for (int i = 0; i < PyList_Size(filteredData); i++)
	{
		// Get list item: borrowed reference.
		PyObject* element = PyList_GetItem(filteredData, i);
		if (!element)
		{
			// Failure
			if (PyErr_Occurred())
			{
				this->logErrorMessage();
			}
			delete newReadings;

			return NULL;
		}

		// Get 'asset_code' value: borrowed reference.
		PyObject* assetCode = PyDict_GetItemString(element,
							   "asset_code");
		// Get 'reading' value: borrowed reference.
		PyObject* reading = PyDict_GetItemString(element,
							 "reading");

		// Keys not found or reading is not a dict
		if (!assetCode ||
		    !reading ||
		    !PyDict_Check(reading))
		{
			// Failure
			if (PyErr_Occurred())
			{
				this->logErrorMessage();
			}
			delete newReadings;

			return NULL;
		}

		// Fetch all Datapoins in 'reading' dict			
		PyObject *dKey, *dValue;
		Py_ssize_t dPos = 0;
		Reading* newReading = NULL;

		// Fetch all Datapoins in 'reading' dict
		// dKey and dValue are borrowed references
		while (PyDict_Next(reading, &dPos, &dKey, &dValue))
		{
			DatapointValue* dataPoint;
			if (PyInt_Check(dValue) || PyLong_Check(dValue))
			{
				dataPoint = new DatapointValue((long)PyInt_AsUnsignedLongMask(dValue));
			}
			else if (PyFloat_Check(dValue))
			{
				dataPoint = new DatapointValue(PyFloat_AS_DOUBLE(dValue));
			}
			else if (PyString_Check(dValue))
			{
				dataPoint = new DatapointValue(string(PyString_AsString(dValue)));
			}
			else
			{
				delete newReadings;
				delete dataPoint;

				return NULL;
			}

			// Add / Update the new Reading data			
			if (newReading == NULL)
			{
				newReading = new Reading(PyString_AsString(assetCode),
							 new Datapoint(PyString_AsString(dKey),
								       *dataPoint));
			}
			else
			{
				newReading->addDatapoint(new Datapoint(PyString_AsString(dKey),
								       *dataPoint));
			}

			/*
			 * Set id, ts and user_ts of the original data
			 */
			// Get 'id' value: borrowed reference.
			PyObject* id = PyDict_GetItemString(element, "id");
			if (id && PyLong_Check(id))
			{
				// Set id
				newReading->setId(PyLong_AsUnsignedLong(id));
			}

			// Get 'ts' value: borrowed reference.
			PyObject* ts = PyDict_GetItemString(element, "ts");
			if (ts && PyLong_Check(ts))
			{
				// Set timestamp
				newReading->setTimestamp(PyLong_AsUnsignedLong(ts));
			}

			// Get 'user_ts' value: borrowed reference.
			PyObject* uts = PyDict_GetItemString(element, "user_ts");
			if (uts && PyLong_Check(uts))
			{
				// Set user timestamp
				newReading->setUserTimestamp(PyLong_AsUnsignedLong(uts));
			}

			// Remove temp objects
			delete dataPoint;
		}

		if (newReadings)
		{
			// Add the new reading to result vector
			newReadings->push_back(newReading);
		}
	}

	return newReadings;
}

/**
 * Log current Python 2.7 error message
 *
 */
void Python27Filter::logErrorMessage()
{
	//Get error message
	PyObject *pType, *pValue, *pTraceback;
	PyErr_Fetch(&pType, &pValue, &pTraceback);

	// NOTE from :
	// https://docs.python.org/2/c-api/exceptions.html
	//
	// The value and traceback object may be NULL
	// even when the type object is not.	
	const char* pErrorMessage = pValue ?
				    PyString_AsString(pValue) :
				    "no error description.";

	Logger::getLogger()->fatal("Filter '%s', script "
				   "'%s': Error '%s'",
				   this->getName().c_str(),
				   m_pythonScript.c_str(),
				   pErrorMessage ?
				   pErrorMessage :
				   "no description");

	// Reset error
	PyErr_Clear();

	// Remove references
	Py_CLEAR(pType);
	Py_CLEAR(pValue);
	Py_CLEAR(pTraceback);
}

bool Python27Filter::configure()
{
	// Import script as module
	// NOTE:
	// Script file name is:
	// lowercase(categoryName) + _script_ + methodName + ".py"

	// 1) Get methodName
	std::size_t found = m_pythonScript.rfind(PYTHON_SCRIPT_METHOD_PREFIX);
	string filterMethod = m_pythonScript.substr(found + strlen(PYTHON_SCRIPT_METHOD_PREFIX));
	// Remove .py from filterMethod
	found = filterMethod.rfind(PYTHON_SCRIPT_FILENAME_EXTENSION);
	filterMethod.replace(found,
			     strlen(PYTHON_SCRIPT_FILENAME_EXTENSION),
			     "");
	// Remove .py from pythonScript
	found = m_pythonScript.rfind(PYTHON_SCRIPT_FILENAME_EXTENSION);
	m_pythonScript.replace(found,
			     strlen(PYTHON_SCRIPT_FILENAME_EXTENSION),
			     "");

	// 2) Import Python script
	PyObject* pName = PyString_FromString(m_pythonScript.c_str());
	m_pModule = PyImport_Import(pName);
	// Delete pName reference
	Py_CLEAR(pName);

	// Check whether the Python module has been imported
	if (!m_pModule)
	{
		// Failure
		if (PyErr_Occurred())
		{
			this->logErrorMessage();
		}
		Logger::getLogger()->fatal("Filter '%s' (%s), cannot import Python 2.7 script "
					   "'%s' from '%s'",
					   this->getName().c_str(),
					   this->getConfig().getName().c_str(),
					   m_pythonScript.c_str(),
					   this->getFiltersPath().c_str());

		// This will abort the filter pipeline set up
		return false;
	}

	// NOTE:
	// Filter method to call is the same as filter name
	// Fetch filter method in loaded object
	m_pFunc = PyObject_GetAttrString(m_pModule, filterMethod.c_str());

	if (!PyCallable_Check(m_pFunc))
	{
		// Failure
		if (PyErr_Occurred())
		{
			this->logErrorMessage();
		}

		Logger::getLogger()->fatal("Filter %s (%s) error: cannot find Python 2.7 method "
					   "'%s' in loaded module '%s.py'",
					   this->getName().c_str(),
					   this->getConfig().getName().c_str(),
					   filterMethod.c_str(),
					   m_pythonScript.c_str());
		Py_CLEAR(m_pModule);
		m_pModule = NULL;
		Py_CLEAR(m_pFunc);
		m_pFunc = NULL;

		// This will abort the filter pipeline set up
		return false;
	}

	// Whole configuration as it is
	string filterConfiguration;

	// Get 'config' filter category configuration
	if (this->getConfig().itemExists("config"))
	{
		filterConfiguration = this->getConfig().getValue("config");
	}
	else
	{
		// Set empty object
		filterConfiguration = "{}";
	}

	/**
	 * We now pass the filter JSON configuration to the loaded module
	 */
	PyObject* pConfigFunc = PyObject_GetAttrString(m_pModule,
						       (char *)string(DEFAULT_FILTER_CONFIG_METHOD).c_str());

	// Check whether "set_filter_config" method exists
	if (PyCallable_Check(pConfigFunc))
	{
		// Set configuration object     
		PyObject* pConfig = PyDict_New();
		// Add JSON configuration, as string, to "config" key
		PyObject* pConfigObject = PyString_FromString(filterConfiguration.c_str());
		PyDict_SetItemString(pConfig,
				     "config",
				     pConfigObject);
		Py_CLEAR(pConfigObject);
		/**
		 * Call method set_filter_config(c)
		 * This creates a global JSON configuration
		 * which will be available when fitering data with "plugin_ingest"
		 *
		 * set_filter_config(config) returns 'True'
		 */
		PyObject* pSetConfig = PyObject_CallFunctionObjArgs(pConfigFunc,
								    // arg 1
								    pConfig,
								    // end of args
								    NULL);
		// Check result
		if (!pSetConfig ||
		    !PyBool_Check(pSetConfig) ||
		    !PyInt_AsLong(pSetConfig))
		{
			this->logErrorMessage();

			Py_CLEAR(m_pModule);
			delete m_pModule;
			Py_CLEAR(m_pFunc);
			delete m_pFunc;

			// Remove temp objects
			Py_CLEAR(pConfig);
			Py_CLEAR(pSetConfig);

			// Remove func object
			Py_CLEAR(pConfigFunc);

			return false;
		}

		// Remove temp objects
		Py_CLEAR(pSetConfig);
		Py_CLEAR(pConfig);
	}
	else
	{
		// Reset error if function is not present
		PyErr_Clear();
	}

	// Remove func object
	Py_CLEAR(pConfigFunc);

	return true;
}

/**
 * Set the Python script name to load.
 *
 * If the attribute "file" of "script" items exists
 * in input configuration the m_pythonScript member is updated.
 *
 * This method must be called before Python35Filter::configure()
 *
 * @return	True if script file exists, false otherwise
 */
bool Python27Filter::setScriptName()
{
	// Check whether we have a Python 2.7 script file to import
	if (this->getConfig().itemExists(SCRIPT_CONFIG_ITEM_NAME))
	{
		try
		{
			// Get Python script file from "file" attibute of "script" item
			m_pythonScript =
				this->getConfig().getItemAttribute(SCRIPT_CONFIG_ITEM_NAME,
								   ConfigCategory::FILE_ATTR);
			// Just take file name and remove path
			std::size_t found = m_pythonScript.find_last_of("/");
			m_pythonScript = m_pythonScript.substr(found + 1);
		}
		catch (ConfigItemAttributeNotFound* e)
		{
			delete e;
		}
		catch (exception* e)
		{
			delete e;
		}
	}

	if (m_pythonScript.empty())
	{
		// Do nothing
		Logger::getLogger()->warn("Filter '%s', "
					  "called without a Python 2.7 script. "
					  "Check 'script' item in '%s' configuration. "
					  "Filter has been disabled.",
					  this->getName().c_str(),
					  this->getConfig().getName().c_str());
	}

	return !m_pythonScript.empty();
}

/**
 * Reconfigure Python27 filter with new configuration
 *
 * @param    newConfig		The new configuration
 *				from "plugin_reconfigure"
 * @return			True on success, false on errors.
 */
bool Python27Filter::reconfigure(const string& newConfig)
{
	lock_guard<mutex> guard(m_configMutex);

	// Cleanup Loaded module first
	Py_CLEAR(m_pModule);
	m_pModule = NULL;
	Py_CLEAR(m_pFunc);
	m_pFunc = NULL;
	m_pythonScript.clear();

	// Apply new configuration
	this->setConfig(newConfig);

	// Check script name
	if (!this->setScriptName())
	{
		// Force disable
		this->disableFilter();
		return false;
	}
	return this->configure();
}
