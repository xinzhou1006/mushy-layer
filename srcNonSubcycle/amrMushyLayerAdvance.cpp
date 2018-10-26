#include "amrMushyLayer.H"

#include "computeNorm.H"
#include "Gradient.H"
#include "Divergence.H"
#include "AMRTGA.H"
#include "CH_assert.H"
#include "MultilevelLinearOp.H"
#include "RelaxSolver.H"
#include "CellToEdge.H"
#include "ExtrapFillPatch.H"
#include "VelBCHolder.H"
#include "PiecewiseLinearFillPatchFace.H"
#include "SetValLevel.H"
#include "EdgeToCell.H"
#include "CoarseAverageFace.H"


void
amrMushyLayer::timeStep()
{
	CH_TIME("amrMushyLayer::timestep()");

	if (m_parameters.physicalProblem == m_enforcedPorosity)
	{
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		LevelData<FArrayBox>& levelPhiNew = *(m_scalarNew[m_porosity][lev]);
		DataIterator levelDit = levelPhiNew.dataIterator();
		for (levelDit.begin(); levelDit.ok(); ++levelDit)
		{
			FArrayBox& porosity = levelPhiNew[levelDit()];
			FArrayBox& solidFrac = (*m_scalarNew[m_solidFraction][lev])[levelDit()];
			Box thisBox = porosity.box();
			IntVect box_iv = thisBox.smallEnd();

			BoxIterator bit(thisBox);
			for (bit.begin(); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				RealVect loc;
				getLocation(iv, lev, loc);
				Real x = loc[0];
				Real z = loc[1];

				porosity(iv) = timeDepPorosity(x,z,m_time);
				solidFrac(iv) = 1-porosity(iv);
			}
		}
	}
	}

	if (s_verbosity >=2)
	{
		pout() << "Timestep " << m_cur_step
				<< " Advancing solution from time "
				<< m_time << " with dt = " << m_dt << endl;
		pout() << "Finest level = " << m_finest_level << endl;
	}
	// copy new data to old data
	for (int a_var=0; a_var<m_numVars; a_var++)
	{
		assert (m_scalarOld[a_var].size() == m_scalarNew[a_var].size());
		activeLevelCopy(m_scalarNew[a_var], m_scalarOld[a_var]);
	}

	//Generate vectors of grids and variables which only span the active levels
	//This is the data structure that AMRElliptic expects
	Vector<Vector<RefCountedPtr<LevelData<FArrayBox> > > > a_scalarNew, a_scalarOld;

	Vector<DisjointBoxLayout> a_amrGrids;
	getActiveGridsVars(a_scalarNew, a_scalarOld, a_amrGrids);

	//We need a different data structure for the BE/TGA solver (this is a bit silly)
	Vector<LevelData<FArrayBox>* >  a_thetaNew, a_thetaOld, a_thetaDiff, a_thetaOldTemp, a_ThetaLNew, a_ThetaLOld;

	//Our guess of theta^(n+1) at the previous iteration, used to check for convergence of the iteration
	Vector<LevelData<FArrayBox>* > a_thetaNewPrev;
	Vector<RefCountedPtr<LevelData<FArrayBox> > > Theta_prev, H_prev;

	/// values at n-2 timestep, for predictor in non-linear update
	Vector<RefCountedPtr<LevelData<FArrayBox> > > H_twoPrev, Theta_twoPrev;


	a_thetaNew.resize(m_finest_level+1, NULL);
	a_thetaOld.resize(m_finest_level+1, NULL);
	a_thetaNewPrev.resize(m_finest_level+1, NULL);
	Theta_prev.resize(m_finest_level+1);
	H_prev.resize(m_finest_level+1);
	Theta_twoPrev.resize(m_finest_level+1);
	H_twoPrev.resize(m_finest_level+1);

	a_ThetaLNew.resize(m_finest_level+1, NULL);
	a_ThetaLOld.resize(m_finest_level+1, NULL);

	//Keep these in case they're useful for more debugging
	a_thetaDiff.resize(m_finest_level+1, NULL);
	a_thetaOldTemp.resize(m_finest_level+1, NULL);

	IntVect zeroGhost = IntVect::Zero;

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		a_thetaNew[lev] = &(*a_scalarNew[m_theta][lev]);
		a_thetaOld[lev] = &(*a_scalarOld[m_theta][lev]);

		a_thetaNewPrev[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect);
		a_thetaNew[lev]->copyTo(*a_thetaNewPrev[lev]);

		a_ThetaLNew[lev] = &(*a_scalarNew[m_compositionLiquid][lev]);
		a_ThetaLOld[lev] = &(*a_scalarOld[m_compositionLiquid][lev]);

		Theta_prev[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));
		m_scalarNew[m_composition][lev]->copyTo(*Theta_prev[lev]);

		H_prev[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));
		m_scalarNew[m_enthalpy][lev]->copyTo(*H_prev[lev]);

		H_twoPrev[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));
		m_scalarNew[m_enthalpy][lev]->copyTo(*H_twoPrev[lev]);

		Theta_twoPrev[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));
		m_scalarNew[m_composition][lev]->copyTo(*Theta_twoPrev[lev]);

	}


	logMessage(8, "    amrMushyLayer::timestep() - setup solvers");


	Vector<LevelData<FArrayBox>* > thetaSource(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > ThetaLSource(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > zeroSource(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > ThetaDiffusion(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > V_gradH_n(m_finest_level+1,NULL);

	Vector<LevelData<FArrayBox>* > V_dThetadz_n(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > ThetaDiffusion_n(m_finest_level+1,NULL);


	for (int lev=0; lev<=m_finest_level; lev++)
	{
		const DisjointBoxLayout& grids = m_amrGrids[lev];
		thetaSource[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		ThetaLSource[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		zeroSource[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		V_gradH_n[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		V_dThetadz_n[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		ThetaDiffusion_n[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);

		DataIterator dit = thetaSource[lev]->dataIterator();
		for (dit.begin(); dit.ok(); ++dit)
		{
			(*thetaSource[lev])[dit()].setVal(0);
			(*ThetaLSource[lev])[dit()].setVal(0);
			(*zeroSource[lev])[dit()].setVal(0);
			(*V_gradH_n[lev])[dit()].setVal(0);
			(*V_dThetadz_n[lev])[dit()].setVal(0);
			(*ThetaDiffusion_n[lev])[dit()].setVal(0);

			(*a_thetaNewPrev[lev])[dit].setVal(0);
			(*a_thetaNewPrev[lev])[dit] += (*a_thetaNew[lev])[dit];

		}
	}




	//Setup solvers for this timestep
	setupAdvectionSolvers();
	setupPoissonSolvers(a_thetaNew, thetaSource, a_ThetaLNew, zeroSource, a_amrGrids);

	//Must be done after setupPoissonSolvers()
	setupMultigrid(a_thetaNew, thetaSource, a_ThetaLNew, zeroSource, a_amrGrids);

	///Calculate V*dH/dz at time n
	if (m_frameAdvectionMethod == m_finiteDifference)
	{
		calculateFrameAdvection(m_enthalpy, V_gradH_n, m_parameters.nonDimVel);
		calculateFrameAdvection(m_composition, V_dThetadz_n, m_parameters.nonDimVel);
	}
	else if(m_frameAdvectionMethod == m_godunov)
	{
		calculateFrameAdvectionGodunov(m_enthalpy);
		calculateFrameAdvectionGodunov(m_composition);
	}
	else
	{
		MayDay::Error("Unrecognized m_frameAdvectionMethod specified");
	}

	// Now we have our static source terms, start iterating
	Real residualEnthalpy = 10, residualConc = 10;
	int iteration = 0;
	Vector<Real> heatResiduals, concResiduals;
	bool converged = false;

	logMessage(8, "amrMushyLayer::timestep() - begin nonlinear solve");

	// Keep iterating until they have both converged
	while (!converged)
	{
		converged = nonlinearSolver(a_scalarNew,a_scalarOld, a_amrGrids,
				a_thetaNew, a_thetaOld,
				a_thetaDiff, a_thetaOldTemp,
				a_ThetaLNew, a_ThetaLOld, a_thetaNewPrev,
				zeroSource, V_gradH_n, V_dThetadz_n,
				thetaSource, ThetaLSource,
				Theta_prev, H_prev,
				Theta_twoPrev, H_twoPrev,
				residualEnthalpy,
				residualConc,
				heatResiduals, concResiduals,
				iteration);

		// This seems to help.
		//		averageCoarseToFineSolutions();

	} // End while (residual > tolerance) loop

	//Post timestep operations:


	//Do refluxing if we've been doing advection
	//				if (m_parameters.problemType != m_bmDiffusion and m_parameters.problemType != m_bmDiffusiveSolidification)
	//				{
	//					for (int lev=0; lev<m_finest_level; lev++)
	//					{
	//						doReflux(m_compositionLiquid, lev);
	//					}
	//
	//				}

	//For debugging
	//Turn off to speed up run time
	//	calculateThetaDiffusion(ThetaDiffusion_n);

	// average solutions down to coarser levels
	averageCoarseToFineSolutions();


	// finally, update to new time and increment current step
	m_time += m_dt;
	m_cur_step += 1;

	// clean up temp storage


	for (int lev=0; lev<=m_finest_level; lev++)
	{

		if (a_thetaNewPrev[lev] != NULL)
		{
			delete a_thetaNewPrev[lev];
			a_thetaNewPrev[lev] = NULL;
		}
		if (thetaSource[lev] != NULL)
		{
			delete thetaSource[lev];
			thetaSource[lev] = NULL;
		}
		if (zeroSource[lev] != NULL)
		{
			delete zeroSource[lev];
			zeroSource[lev] = NULL;
		}
		if (ThetaLSource[lev] != NULL)
		{
			delete ThetaLSource[lev];
			ThetaLSource[lev] = NULL;
		}
		if (ThetaDiffusion[lev] != NULL)
		{
			delete ThetaDiffusion[lev];
			ThetaDiffusion[lev] = NULL;
		}
		if (V_gradH_n[lev] != NULL)
		{
			delete V_gradH_n[lev];
			V_gradH_n[lev] = NULL;
		}
		if (V_dThetadz_n[lev] != NULL)
		{
			delete V_dThetadz_n[lev];
			V_dThetadz_n[lev] = NULL;
		}
		if (ThetaDiffusion_n[lev] != NULL)
		{
			delete ThetaDiffusion_n[lev];
			ThetaDiffusion_n[lev] = NULL;
		}
	}

	pout () << "amrMushyLayer::timestep " << m_cur_step
			<< ",      end time = "
			<< setiosflags(ios::fixed) << setprecision(6) << setw(12) << m_time << "( " << m_time*m_parameters.timescale << " secs) "
			<< ", dt = "
			<< setiosflags(ios::fixed) << setprecision(6) << setw(12) << m_dt << "( " << m_dt * m_parameters.timescale << " secs) "
			<< endl;


	int totalCellsAdvanced = 0;
	for (int lev=0; lev<m_num_cells.size(); lev++)
	{
		totalCellsAdvanced += m_num_cells[lev];
	}

	//	pout() << "Time = " << m_time << " cells advanced = "
	//			<< totalCellsAdvanced << endl;
	for (int lev=0; lev<m_num_cells.size(); lev++)
	{
		//		pout () << "Time = " << m_time
		//				<< "  level " << lev << " cells advanced = "
		//				<< m_num_cells[lev] << endl;
	}


}

bool amrMushyLayer::nonlinearSolver(Vector<Vector<RefCountedPtr<LevelData<FArrayBox> > > > a_scalarNew,
		Vector<Vector<RefCountedPtr<LevelData<FArrayBox> > > >a_scalarOld,
		Vector<DisjointBoxLayout> a_amrGrids,
		Vector<LevelData<FArrayBox>* >  a_thetaNew,
		Vector<LevelData<FArrayBox>* > a_thetaOld,
		Vector<LevelData<FArrayBox>* > a_thetaDiff,
		Vector<LevelData<FArrayBox>* > a_thetaOldTemp,
		Vector<LevelData<FArrayBox>* > a_ThetaLNew,
		Vector<LevelData<FArrayBox>* > a_ThetaLOld,
		Vector<LevelData<FArrayBox>* > a_thetaNewPrev,
		Vector<LevelData<FArrayBox>* > zeroSource,
		Vector<LevelData<FArrayBox>* > V_gradH_n,
		Vector<LevelData<FArrayBox>* > V_dThetadz_n,
		Vector<LevelData<FArrayBox>* > thetaSource,
		Vector<LevelData<FArrayBox>* > ThetaLSource,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > a_ThetaPrevIteration,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > a_HPrevIteration,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > C_twoPrev,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > H_twoPrev,
		Real residualEnthalpy,
		Real residualConc,
		Vector<Real> &heatResiduals, Vector<Real> &concResiduals,
		int &iteration)
{
	CH_TIME("amrMushyLayer::nonLinearSolver()");

	//Save plotfiles for this iteration
	if (iteration % m_iteration_plot_interval == 0 && m_iteration_plot_interval > -1)
	{
		writePlotFile(iteration);
	}

	int lbase = 0;
	int lmax = m_finest_level;

	Real initialResidualEnthalpy = 10, initialResidualConc = 10;
	if (heatResiduals.size() > 0)
	{
		initialResidualEnthalpy = heatResiduals[0];
		initialResidualConc = concResiduals[0];
	}

	// Get custom parameters if specified
	Real absTolerance = 0.01, relTolerance = 1e5;
	ParmParse ppPicard("picardIterator");
	ppPicard.query("absTolerance", absTolerance);
	ppPicard.query("relTolerance", relTolerance);

	// Initial iteration is 0
	int iterationLimit = 200;
	ppPicard.query("iterationLimit", iterationLimit);

	// When we calculate variables via the enthalpy method, we don't want to
	// recalculate theta as this has already been found implicitly
	vector<int> ignore, ignoreNone, ignoreExcepttheta;
	ignore.push_back(m_theta);
	ignoreExcepttheta.push_back(m_theta);

	// Coefficients can change after each iteration
	setupPoissonSolvers(a_thetaNew, thetaSource, a_ThetaLNew, zeroSource, a_amrGrids);

	//Do fluid advection
	//Get fluid velocities based on our current best estimate of the
	//fields at time n+1/2
	if (m_parameters.physicalProblem != m_bmDiffusiveSolidification && m_parameters.physicalProblem != m_bmDiffusion)
	{
		Real timeInterpolant = 0.5;
		updateVelocityComp(a_amrGrids, timeInterpolant);
	}



	//Calculate U.grad(theta) and put it in m_dScalar[m_theta]
	if (m_parameters.physicalProblem != m_bmDiffusiveSolidification && m_parameters.physicalProblem != m_bmDiffusion)
	{
		makeAdvectionTerm(m_theta, zeroSource, m_fluidAdv, m_dScalar[m_theta]);
	}
	else
	{
		for (int lev=0; lev<=m_finest_level; lev++)
		{
			for (DataIterator dit=m_dScalar[m_theta][lev]->dataIterator(); dit.ok(); ++dit)
			{
				(*m_dScalar[m_theta][lev])[dit].setVal(0.0);
			}
		}
	}
	//	makeAdvectionTerm(m_compositionLiquid, zeroSource, m_fluidAdv, m_dScalar[m_compositionLiquid]);

	//Solve for theta
	calculatethetaSourceTerm(thetaSource, V_gradH_n);

	// If we're just doing advection, add on the advection part explicitly
	if (m_parameters.physicalProblem == m_bmAdvection)
	{

		for (int lev=0; lev<=m_finest_level; lev++)
		{
			for (DataIterator dit = m_scalarNew[m_theta][lev]->dataIterator(); dit.ok(); ++dit)
			{
				(*m_dScalar[m_theta][lev])[dit] *= m_dt;
				(*m_scalarNew[m_theta][lev])[dit] += (*m_dScalar[m_theta][lev])[dit];
			}
		}
	}
	//Otherwise, do an implicit timestep with advection as a source term for the
	//elliptic solver
	else
	{

		if (m_timeIntegrationOrder == 1)
		{
			m_diffuseBEtheta->oneStep(a_thetaNew,
					a_thetaOld,
					thetaSource,
					m_dt,
					lbase,
					lmax,
					false); //don't set phi=zero
		}
		else if(m_timeIntegrationOrder == 2)
		{
			m_diffuseTGAtheta->setTime(m_time);
			m_diffuseTGAtheta->resetAlphaAndBeta(1,1); //think this might be unnecessary


			m_diffuseTGAtheta->oneStep(a_thetaNew,
					a_thetaOld,
					thetaSource,
					m_dt,
					lbase,
					lmax,
					m_time);
		}
		else
		{
			MayDay::Error("amrMushyLayer::timestep() - Invalid m_timeIntegrationOrder");
		}
	}

	//Do refluxing if we've been doing advection
	if (m_parameters.physicalProblem != m_bmDiffusion and m_parameters.physicalProblem != m_bmDiffusiveSolidification)
	{
		for (int lev=0; lev<m_finest_level; lev++)
		{
			doReflux(m_theta, lev);
		}

	}


	logMessage(8, "    amrMushyLayer::timestep() - update enthalpy variables");



	if (m_parameters.physicalProblem != m_bmHRL && m_parameters.physicalProblem != m_bmAdvection
			&& m_parameters.physicalProblem != m_bmDiffusion
			&& m_parameters.physicalProblem != m_enforcedPorosity)
	{
		logMessage(8, "    amrMushyLayer::timestep() - advance Theta");

		calculateThetaLSourceTerm(V_dThetadz_n, ThetaLSource);

		m_diffuseBEThetaL->oneStep(a_ThetaLNew,
				a_ThetaLOld,
				ThetaLSource,
				m_dt,
				lbase,
				lmax,
				false); //don't set phi=zero
		ignore.push_back(m_compositionLiquid);

		if(m_timeIntegrationOrder == 2)
		{
			logMessage(8, "WARNING - not doing TGA update for Theta");
		}

		//Do refluxing if we've been doing advection
		if (m_parameters.physicalProblem != m_bmDiffusion and m_parameters.physicalProblem != m_bmDiffusiveSolidification)
		{
			for (int lev=0; lev<m_finest_level; lev++)
			{
				doReflux(m_compositionLiquid, lev);
			}

		}

	}


	// Update H
	calculateEnthalpy();

	if (m_parameters.physicalProblem != m_bmHRL && m_parameters.physicalProblem != m_bmAdvection
			&& m_parameters.physicalProblem != m_enforcedPorosity)
	{
		// Update porosity and solute concentration in the solid
		// Don't update temperature of liquid concentration, as we have found these ourselves
		updateEnthalpyVariables(ignore);

		// Update the bulk composition

		//Shouldn't need this if clause, should do this for solidificationt oo
		if (m_parameters.physicalProblem != m_bmDiffusiveSolidification)
		{
			calculateBulkConcentration();
		}
	}


	// For the diffusion problem we just care about theta, which we've already got explicitly
	if (m_parameters.physicalProblem != m_bmDiffusion
			and m_parameters.physicalProblem != m_bmHRL
			and m_parameters.physicalProblem != m_bmAdvection
			&& m_parameters.physicalProblem != m_enforcedPorosity)
	{
		updateEnthalpyVariables(ignoreNone);
	}

	//	if (m_parameters.problemType == m_bmHRL)
	//	{
	//		// Recalculate temperature
	//		vector<int> ignoreSpecific;
	//		ignoreSpecific.push_back(m_porosity);
	//		updateEnthalpyVariables(ignoreSpecific);
	//	}


	// 3. Check the residual
	logMessage(8, "    amrMushyLayer::timestep() - calculate residual");

	residualEnthalpy = calculateResidual2(a_HPrevIteration, m_scalarNew[m_enthalpy]);
	residualConc = calculateResidual2(a_ThetaPrevIteration, m_scalarNew[m_composition]);
	heatResiduals.push_back(residualEnthalpy);
	concResiduals.push_back(residualConc);


	bool enthalpyConverged = (abs(residualEnthalpy) < absTolerance or abs(residualEnthalpy) < initialResidualEnthalpy/relTolerance);
	bool concConverged = (abs(residualConc) < absTolerance     or abs(residualConc) < initialResidualConc/relTolerance);

	//Make sure we don't kepe iterating if we're just doing advection
	if (m_parameters.physicalProblem == m_bmAdvection)
	{
		enthalpyConverged = true;
	}

	// If we haven't converged, try and be clever
	if (not enthalpyConverged or not concConverged)
	{
		/*
		 * Now let's try and do some clever stuff to speed up the convergence of the nonlinear method
		 * First wait until we've done 2 iterations, then look at the two residuals. Three cases:
		 * 1. One residual is negative, the other is positive -> take the average of H and Theta between the two iterations
		 * 2. Both residuals same sign, the second closer to 0 -> good, we've converging, do a linear extrapolation for H and Theta
		 * 3. Both residuals same sign, but second further from zero -> oh dear, we're diverging. Don't do anything and hope things get better
		 */

		Vector<RefCountedPtr<LevelData<FArrayBox> > > H_workings, C_workings;
		H_workings.resize(m_finest_level+1); C_workings.resize(m_finest_level+1);

		for (int lev=0; lev<=m_finest_level; lev++)
		{
			H_workings[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, IntVect::Zero));
			C_workings[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, IntVect::Zero));
		}


		int initialIters = 999;
		Real predictorLimit = 0; // This reduces how greatly we change the solution - we can't be too aggressive or we lose the true solution
		ppPicard.query("initialIters", initialIters);
		ppPicard.query("predictorLimit", predictorLimit);

		// If the two residuals have an opposite sign
		if (iteration > 999 and
				heatResiduals[iteration]*heatResiduals[iteration-1] < 0
				and concResiduals[iteration]*concResiduals[iteration-1] < 0)
		{
			//We should never be using more of the previous iteration than the current one
			Real max_weighting = 1;
			Real H_weighting = abs(heatResiduals[iteration-1])/abs(heatResiduals[iteration]);
			Real Theta_weighting = abs(concResiduals[iteration-1])/abs(concResiduals[iteration]);

			H_weighting = min(H_weighting, max_weighting);
			Theta_weighting = min(Theta_weighting, max_weighting);

			//Average enthalpy and concentration, then recalculate enthalpy variables
			for (int lev=0; lev<=m_finest_level; lev++)
			{
				a_HPrevIteration[lev]->copyTo(*H_workings[lev]);
				a_ThetaPrevIteration[lev]->copyTo(*C_workings[lev]);

				for (DataIterator dit=m_scalarNew[m_enthalpy][lev]->dataIterator(); dit.ok(); ++dit)
				{

					(*H_workings[lev])[dit] *= H_weighting;
					(*m_scalarNew[m_enthalpy][lev])[dit] += (*H_workings[lev])[dit];
					(*m_scalarNew[m_enthalpy][lev])[dit] /= 2;

					(*C_workings[lev])[dit] *= Theta_weighting;
					(*m_scalarNew[m_composition][lev])[dit] += (*C_workings[lev])[dit];
					(*m_scalarNew[m_composition][lev])[dit] /= 2;
				}

				updateEnthalpyVariables(ignoreNone);
			}

		}
		//Residuals have the same sign, and they're getting smaller
		else if(iteration > initialIters and
				abs(heatResiduals[iteration]) < abs(heatResiduals[iteration-1]) )
		{
			//Predict H and Theta by linear extrapolation
			Vector<RefCountedPtr<LevelData<FArrayBox> > > alpha_H, alpha_C;
			alpha_H.resize(m_finest_level+1);
			alpha_C.resize(m_finest_level+1);

			//			Real H_gradient = predictorScaling; // * heatResiduals[iteration]/(heatResiduals[iteration]-heatResiduals[iteration-1]);
			//			Real Theta_gradient = predictorScaling; // * concResiduals[iteration]/(concResiduals[iteration]-concResiduals[iteration-1]);
			IntVect zeroGhost = IntVect::Zero;

			for (int lev=0; lev<=m_finest_level; lev++)
			{
				//				Interval interv(0,0);
				alpha_H[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));
				//				m_scalarNew[m_enthalpy][lev]->copyTo(interv, *alpha_H[lev], interv);

				alpha_C[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(m_amrGrids[lev], 1, zeroGhost));

				//				m_scalarNew[m_composition][lev]->copyTo(interv, *alpha_C[lev], interv);

				//H_workings contains H^n - H^{n-2}
				//H_predictor contains - (H^n - H^{n-1} / H_workings)

				//				m_scalarNew[m_enthalpy][lev]->copyTo(*H_workings[lev]);
				//				m_scalarNew[m_composition][lev]->copyTo(*C_workings[lev]);

				for (DataIterator dit=m_scalarNew[m_enthalpy][lev]->dataIterator(); dit.ok(); ++dit)
				{
					//alpha_H = H^n
					(*alpha_H[lev])[dit].copy((*m_scalarNew[m_enthalpy][lev])[dit]);
					(*alpha_C[lev])[dit].copy((*m_scalarNew[m_composition][lev])[dit]);

					//H_workings = H^n - H^{n-2}
					(*H_workings[lev])[dit].copy((*m_scalarNew[m_enthalpy][lev])[dit]);
					(*H_workings[lev])[dit] -= (*H_twoPrev[lev])[dit];
					(*C_workings[lev])[dit].copy((*m_scalarNew[m_composition][lev])[dit]);
					(*C_workings[lev])[dit] -= (*C_twoPrev[lev])[dit];


					//alpha_H = H^n - H^{n-1}
					(*alpha_H[lev])[dit] -= (*a_HPrevIteration[lev])[dit];
					(*alpha_C[lev])[dit] -= (*a_ThetaPrevIteration[lev])[dit];

					(*alpha_H[lev])[dit].divide((*H_workings[lev])[dit]);
					(*alpha_C[lev])[dit].divide((*C_workings[lev])[dit]);

					//I think this is wrong
					//					(*alpha_H[lev])[dit].mult(-1);
					//					(*alpha_C[lev])[dit].mult(-1);

					removeNanValues((*alpha_C[lev])[dit]);
					removeNanValues((*alpha_H[lev])[dit]);



					//Ensure that  |alpha| is never greater than predictorLimit (nominally 0.5)
					Real H_scaling = abs( predictorLimit / (*alpha_H[lev])[dit].norm(0,0,1) );
					Real C_scaling = abs( predictorLimit / (*alpha_C[lev])[dit].norm(0,0,1) );
					if (H_scaling < 1)
					{
						(*alpha_H[lev])[dit].mult(H_scaling);
					}
					if (C_scaling < 1)
					{
						(*alpha_C[lev])[dit].mult(C_scaling);
					}


					// Note the minus sign here!
					//					(*alpha_H[lev])[dit].setVal(-limit);
					//					(*alpha_C[lev])[dit].setVal(-limit);


					//Find the bit to add to our current guess
					//H^* = H^n + alpha*(H^n-H^n-1)
					(*H_workings[lev])[dit].copy((*m_scalarNew[m_enthalpy][lev])[dit]);
					(*H_workings[lev])[dit] -= (*a_HPrevIteration[lev])[dit];
					(*H_workings[lev])[dit].mult((*alpha_H[lev])[dit]);

					(*C_workings[lev])[dit].copy((*m_scalarNew[m_composition][lev])[dit]);
					(*C_workings[lev])[dit] -= (*a_ThetaPrevIteration[lev])[dit];
					(*C_workings[lev])[dit].mult((*alpha_C[lev])[dit]);

					removeNanValues((*C_workings[lev])[dit]);
					removeNanValues((*H_workings[lev])[dit]);


					//Add it to our current guess
					(*m_scalarNew[m_enthalpy][lev])[dit] += (*H_workings[lev])[dit];
					(*m_scalarNew[m_composition][lev])[dit] += (*C_workings[lev])[dit];
				}

				updateEnthalpyVariables(ignoreNone);
			}


		}


		//Recalculate residual
		residualEnthalpy = calculateResidual2(a_HPrevIteration, m_scalarNew[m_enthalpy]);
		residualConc = calculateResidual2(a_ThetaPrevIteration, m_scalarNew[m_composition]);
		heatResiduals[iteration] = residualEnthalpy;
		concResiduals[iteration] = residualConc;

		//Don't do this - even if we think we've now converged, do one more proper iteration to make sure
		//		enthalpyConverged = (abs(residualEnthalpy) < absTolerance or abs(residualEnthalpy) < initialResidualEnthalpy/relTolerance);
		//		concConverged = (abs(residualConc) < absTolerance     or abs(residualConc) < initialResidualConc/relTolerance);

	}




	//Store the first residual to determine relative reduction at
	//future timesteps
	if (iteration == 0)
	{
		initialResidualEnthalpy = residualEnthalpy;
	}




	// Now decide if we should keep iteration or not

	// Note that tnitial iteration is 0
	if (iteration == iterationLimit)
	{
		//		MayDay::Error("Picard iteration has got stuck, aborting");
		logMessage(1, "WARNING - Max number of iterations reached");

		// Return true (i.e. 'converged') and move onto next timestep
		return true;
	}




	Real heatConvergenceRate=0, concConvergenceRate=0;
	if (iteration > 0)
	{
		heatConvergenceRate = (abs(heatResiduals[iteration-1])-abs(heatResiduals[iteration]))/abs(heatResiduals[iteration-1]);
		concConvergenceRate = (abs(concResiduals[iteration-1])-abs(concResiduals[iteration]))/abs(concResiduals[iteration-1]);
	}

	pout()  << "    amrMushyLayer::timestep() Iteration " << iteration
			<< ". Heat Residual = " << residualEnthalpy << " (converged = " << enthalpyConverged << ", rate = " << heatConvergenceRate << ")"
			<< ", Concentration residual = " << residualConc << " (converged = " << concConverged << ", rate = "<< concConvergenceRate << ")" << endl;

	iteration++;
	m_total_num_iterations++;

	//Store data for this iteration
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		a_thetaNew[lev]->copyTo(*a_thetaNewPrev[lev]);

		a_ThetaPrevIteration[lev]->copyTo(*C_twoPrev[lev]);
		a_HPrevIteration[lev]->copyTo(*H_twoPrev[lev]);
		m_scalarNew[m_composition][lev]->copyTo(*a_ThetaPrevIteration[lev]);
		m_scalarNew[m_enthalpy][lev]->copyTo(*a_HPrevIteration[lev]);
	}

	if (enthalpyConverged && concConverged)
	{
		return true;
	}
	else
	{
		return false;
	}

}

void amrMushyLayer::
doReflux(int a_var, int refluxLevel)
{
	if (m_parameters.physicalProblem == m_fullProblem or m_parameters.physicalProblem == m_bmHRL)
	{
		doImplicitReflux(a_var, refluxLevel);
	}
	else if (m_parameters.physicalProblem == m_bmAdvection)
	{
		// explicit Reflux
		Real scale = -1.0/m_amrDx[refluxLevel];
		m_fluxRegister[a_var][refluxLevel]->reflux(*m_scalarNew[a_var][refluxLevel],scale);
	}
}

void
amrMushyLayer::
doImplicitReflux(int a_var, int refluxLevel)
{

	CH_assert(refluxLevel < m_finest_level);

	// now do implicit refluxing
	// Vector of pointers to LevelData of FABS
	Vector<LevelData<FArrayBox>* > refluxCor(m_finest_level+1, NULL);
	Vector<LevelData<FArrayBox>* > refluxRHS(m_finest_level+1, NULL);
	// collectUN: AMR vector containing soln at new time
	Vector<LevelData<FArrayBox>* > collectUN(m_finest_level+1, NULL);

	// loop over levels, allocate storage, set up for AMRSolve
	// if coarser level exists, define it as well for BCs.
	int startlev = Max(refluxLevel-1, 0);

	for (int ilev = startlev; ilev<= m_finest_level; ilev++)
	{
		// rhs has no ghost cells, correction does
		refluxRHS[ilev]  = new LevelData<FArrayBox>(m_amrGrids[ilev], 1, IntVect::Zero);
		refluxCor[ilev]  = new LevelData<FArrayBox>(m_amrGrids[ilev], 1, IntVect::Unit);
		collectUN[ilev]  = &(*m_scalarNew[a_var][ilev]);
		for (DataIterator dit = m_amrGrids[ilev].dataIterator(); dit.ok(); ++dit)
		{
			(*refluxRHS[ilev])[dit()].setVal(0.0);
			(*refluxCor[ilev])[dit()].setVal(0.0);
		}
	} // end loop over levels for setup.

	// now loop over levels and set up RHS
	// note that we don't need to look at finest level here,
	// since it will have no finer level to get a reflux correction
	// from.   Also this starts at refluxLevel instead of startLev since,
	// in the case refluxLevel > 0, refluxLevel-1 is only used for boundary conditions
	for (int lev= refluxLevel; lev < m_finest_level; lev++)
	{
		//TODO: should this be negative or positive?
		Real dxLev = m_amrDx[lev];
		Real refluxScale = 1.0/dxLev;

		m_fluxRegister[a_var][lev]->reflux(*refluxRHS[lev], refluxScale);
	}


	int lbase = refluxLevel;
	int lmax  = m_finest_level;

	// this resets the coeffients including eta, alpha, beta
	Real alpha = 1.0;
	Real beta  = -m_dt;

	Vector<MGLevelOp<LevelData<FArrayBox> >* > ops = m_refluxAMRMG->getAllOperators();
	for (int iop = 0; iop < ops.size(); iop++)
	{
		LevelTGAHelmOp<FArrayBox,FluxBox>* helmop = (LevelTGAHelmOp<FArrayBox,FluxBox>*) ops[iop];
		helmop->setAlphaAndBeta(alpha, beta);
	}

	m_refluxAMRMG->solve(refluxCor, refluxRHS, lmax, lbase);

	for (int lev= refluxLevel; lev <= m_finest_level; lev++)
	{
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*m_scalarNew[a_var][lev])[dit()] += (*refluxCor[lev])[dit()];
		}
	}

	//remember that startlev can be different from refluxLevel
	for (int lev = startlev; lev<= m_finest_level; lev++)
	{
		delete refluxRHS[lev];
		delete refluxCor[lev];
		refluxRHS[lev] = NULL;
		refluxCor[lev] = NULL;
	}
}

void amrMushyLayer::
calculatePermeability(bool oldTime)
{
	CH_TIME("amrMushyLayer::calculatePermeability()");

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit=m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			//			Box box = m_amrGrids[lev][dit]; //This doesn't iterate over ghost cells
			//			Box box = (*m_scalarNew[m_solidFraction][lev])[dit].box(); //This does iterate over ghost cells

			FArrayBox& solid = (*m_scalarNew[m_solidFraction][lev])[dit];

			if (oldTime)
			{
				::calculatePermeability((*m_scalarOld[m_permeability][lev])[dit],
						(*m_scalarOld[m_solidFraction][lev])[dit],
						m_parameters, m_amrDx[lev]);
			}
			else
			{
				::calculatePermeability((*m_scalarNew[m_permeability][lev])[dit],
						(*m_scalarNew[m_solidFraction][lev])[dit],
						m_parameters, m_amrDx[lev]);
			}

			//int temp=0;

		} //data iterator
	}
}
//
void amrMushyLayer::
doExplicitVelocityReflux()
{

	Real refluxScale;

	for (int lev=0; lev<m_finest_level; lev++)
	{
		refluxScale = -1.0/m_amrDx[lev]; // petermc made negative, 7 Dec 07
		m_velFluxReg[lev]->refluxCurl(*m_fluidAdv[lev], refluxScale);
	} // end loop over levels
}


void amrMushyLayer::
doImplicitVelocityReflux(int m_level)
{

	// loop over levels and compute RHS
	Vector<LevelData<FArrayBox>* > refluxRHS(m_finest_level+1,NULL);
	Vector<LevelData<FArrayBox>* > refluxCorr(m_finest_level+1,NULL);
	// this is necessary because while solver only can do
	// one component, levelfluxRegister::reflux can only
	// do all of them at once.
	Vector<LevelData<FArrayBox>* > tempRefluxData(m_finest_level+1,NULL);
	// loop over levels, allocate storage, set up for AMRMultiGrid
	// solve

	Real refluxScale;

	int startLev=m_level;
	// if crser level exists, define it as well for BC's
	if (startLev > 0)
	{
		startLev = startLev-1;
	}

	for (int lev=startLev; lev<=m_finest_level; lev++)
	{
		//		const DisjointBoxLayout& levelGrids = thisNSPtr->newVel().getBoxes();
		const DisjointBoxLayout& levelGrids = m_amrGrids[lev];

		// recall that AMRMultiGrid can only do one component
		// rhs has no ghost cells
		refluxRHS[lev] = new LevelData<FArrayBox>(levelGrids, 1);
		tempRefluxData[lev] = new LevelData<FArrayBox>(levelGrids,
				SpaceDim);
		//soln has one layer of ghost cells
		IntVect ghostVect(D_DECL(1,1,1));
		refluxCorr[lev] = new LevelData<FArrayBox>(levelGrids,1,
				ghostVect);

		// initialize rhs to 0
		DataIterator levelDit = tempRefluxData[lev]->dataIterator();
		LevelData<FArrayBox>& levelRefluxData = *(tempRefluxData[lev]);
		for (levelDit.reset(); levelDit.ok(); ++levelDit)
		{
			levelRefluxData[levelDit()].setVal(0.0);
		}

		// while we're here, do refluxing.
		// (recall that startLev may be coarser than m_level
		// for BC's,
		// however, don't want to do anything to that level)
		if  ((lev >= m_level) && (lev < m_finest_level))
		{
			refluxScale = -1.0/m_amrDx[lev]; // petermc made negative, 7 Dec 07
			m_vectorFluxRegister[m_fluidVel][lev]->reflux(levelRefluxData,
					refluxScale);
		}

//		int temp=0;

	} // end loop over levels

	// coarse BC is 0
	if (m_level > 0)
	{
		LevelData<FArrayBox>& crseBCData = *refluxCorr[m_level-1];
		DataIterator crseDit = crseBCData.dataIterator();
		for (crseDit.reset(); crseDit.ok(); ++crseDit)
		{
			crseBCData[crseDit()].setVal(0.0);
		}
	}

	// set convergence metric to be norm of velocity
	// for now just do component-wise maxnorm
	// compute norm over all directions and then use for
	// all velocity components (in order to be consistent)
	//	Vector<LevelData<FArrayBox>* > vectVel(m_finest_level+1, NULL);
	//	thisNSPtr = this;
	//	for (int lev=m_level; lev<=m_finest_level; lev++)
	//	{
	//		vectVel[lev] = thisNSPtr->m_vel_new_ptr;
	//		if (lev < m_finest_level)
	//		{
	//			thisNSPtr = thisNSPtr->finerNSPtr();
	//		}
	//	}

	int normType = 0;
	Interval allVelComps(0, SpaceDim-1);

	Real velNorm = computeNorm(m_vectorNew[m_fluidVel], m_refinement_ratios,
			m_amrDx[m_level], allVelComps, normType, m_level);

	// now loop over directions
	Interval solverComps(0,0);
	for (int dir=0; dir<SpaceDim; dir++)
	{
		Interval velComps(dir,dir);
		for (int lev=m_level; lev<=m_finest_level; ++lev)
		{
			// copy rhs to single-component holder
			tempRefluxData[lev]->copyTo(velComps,*(refluxRHS[lev]),
					solverComps);
			// initial guess for correction is RHS.
			LevelData<FArrayBox>& levelCorr = *(refluxCorr[lev]);
			DataIterator levelDit = levelCorr.dataIterator();
			for (levelDit.reset(); levelDit.ok(); ++levelDit)
			{
				levelCorr[levelDit()].setVal(0.0);
			}
			refluxRHS[lev]->copyTo(solverComps,*(refluxCorr[lev]),
					solverComps);
		} // end set initial guess

		// now set up solver
		int numLevels = m_finest_level+1;

		// This is a Helmholtz operator
		Real alpha = 1.0;
		/* viscosity - set to 0 as we don't (currently) have a viscous term in our momentum equation
		 * \mathbf{U} = - \nabla P + (Ra_T \theta) \mathbf{k}
		 */
		Real nu = 0.0;

		Real beta = -nu*m_dt;

		AMRPoissonOpFactory viscousOpFactory;
		viscousOpFactory.define(m_amrDomains[0],
				m_amrGrids,
				m_refinement_ratios,
				m_amrDx[0],
				m_physBCPtr->viscousRefluxBC(dir),
				alpha,
				beta);

		RelaxSolver<LevelData<FArrayBox> > bottomSolver;
		bottomSolver.m_verbosity = s_verbosity;

		AMRMultiGrid<LevelData<FArrayBox> > viscousSolver;
		AMRLevelOpFactory<LevelData<FArrayBox> >& viscCastFact = (AMRLevelOpFactory<LevelData<FArrayBox> >&) viscousOpFactory;
		viscousSolver.define(m_amrDomains[0],
				viscCastFact,
				&bottomSolver,
				numLevels);

		viscousSolver.m_verbosity = s_verbosity;

//		int  s_viscous_solver_type = 2;
		Real s_viscous_solver_tol = 1e-7;
		int s_viscous_num_smooth_up = 4;
		int s_viscous_num_smooth_down = 4;

		viscousSolver.m_eps = s_viscous_solver_tol;

		viscousSolver.m_pre  = s_viscous_num_smooth_down;
		viscousSolver.m_post = s_viscous_num_smooth_up;

		// This needs to be fixed when AMRMultiGrid has this
		// capability.
		//
		// viscousSolver.setConvergenceMetric(velNorm, 0);
		viscousSolver.m_convergenceMetric = velNorm;

		// now solve
		viscousSolver.solve(refluxCorr, refluxRHS,
				m_finest_level, m_level,
				false); // don't initialize to zero

		// now increment velocity with reflux correction
		for (int lev=m_level; lev<=m_finest_level; ++lev)
		{
			//			LevelData<FArrayBox>& levelVel = *(m_vectorNew[m_fluidVel][lev]);
			LevelData<FluxBox>& levelVel = *(m_fluidAdv[lev]);
			LevelData<FArrayBox>& levelCorr = *(refluxCorr[lev]);
			DataIterator levelDit = levelCorr.dataIterator();
			for (levelDit.reset(); levelDit.ok(); ++levelDit)
			{
				FArrayBox& levelVelDir = levelVel[levelDit][dir];
				levelVelDir.plus(levelCorr[levelDit()],0,dir,1);
			}
		}
	} // end loop over directions

	// clean up storage
	for (int lev=startLev; lev<=m_finest_level; lev++)
	{
		if (refluxRHS[lev] != NULL)
		{
			delete refluxRHS[lev];
			refluxRHS[lev] = NULL;
		}

		if (refluxCorr[lev] != NULL)
		{
			delete refluxCorr[lev];
			refluxCorr[lev] = NULL;
		}

		if (tempRefluxData[lev] != NULL)
		{
			delete tempRefluxData[lev];
			tempRefluxData[lev] = NULL;
		}
	}
}



void amrMushyLayer::
updateVelocityComp(Vector<DisjointBoxLayout> activeGrids, Real alpha)
{

	logMessage(8, "    amrMushyLayerAdvance::updateVelocity");
	// Need the latest permeability in order to calculate U^*
	calculatePermeability(); //Calculate permeability at new timestep

	if (alpha < 1)
	{
		calculatePermeability(true);//Calculate permeability at old timestep
	}

	//For debugging
	Vector<RefCountedPtr<LevelData<FArrayBox> > > zFluidVel;
	zFluidVel.resize(m_finest_level+1);

	PiecewiseLinearFillPatch fillPatch;

	logMessage(10, "    amrMushyLayerAdvance::updateVelocity - apply BCs and fill ghost cells");
	bool isViscous = false;
	//	VelBCHolder velBC(m_physBCPtr->velFuncBC(isViscous));
	VelBCHolder velBCExtrap(m_physBCPtr->velExtrapBC(isViscous));
	//	EdgeVelBCHolder edgeVelExtrapBC(m_physBCPtr->velExtrapBC(isViscous));
	EdgeVelBCHolder edgeVelBC(m_physBCPtr->edgeVelFuncBC(isViscous));

	//	VelBCHolder velInteriorExtrap(m_physBCPtr->velExtrapInterior(isViscous, 4));

	//Calculate the predicted velocity, U^*
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		zFluidVel[lev] = RefCountedPtr<LevelData<FArrayBox> >
		(new LevelData<FArrayBox>(m_amrGrids[lev], 1,m_ghostVect));
		m_vectorNew[m_fluidVel][lev]->copyTo(Interval(1,1), *zFluidVel[lev], Interval(0,0));

		if (lev > 0)
		{
			fillPatch.define(m_amrGrids[lev], m_amrGrids[lev-1], SpaceDim, m_amrDomains[lev-1], getRefinementRatio(lev-1), m_num_ghost);
		}

		LevelData<FArrayBox>& uStar = *m_vectorNew[m_fluidVelPred][lev];
		LevelData<FArrayBox>& uAnalytic = *m_vectorNew[m_fluidVelAnalytic][lev];
		LevelData<FArrayBox>& gradPErr = *m_vectorNew[m_gradPressureErr][lev];
		LevelData<FArrayBox>& divUstarAnalytic = *m_scalarNew[m_divUstarErr][lev];
		LevelData<FArrayBox>& pressureErr = *m_scalarNew[m_pressureError][lev];
		LevelData<FArrayBox>& thetaAdvectionAnalytic = *m_scalarNew[m_thetaForcingAnalytic][lev];

		logMessage(10, "    amrMushyLayerAdvance::updateVelocity - make U^*");
		for (DataIterator dit = m_vectorNew[m_fluidVelPred][lev]->dataIterator(); dit.ok(); ++dit)
		{
			logMessage(10, "    amrMushyLayerAdvance::updateVelocity - make U^* on grid");

			Box b = uStar[dit].box();

			FArrayBox thetaInit(b, 1);

			for (BoxIterator bit(b); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				RealVect loc;
				getLocation(iv, lev, loc);

				Real permeability, theta, Theta;

				//Time average permeability
				Real permeabilityNew = (*m_scalarNew[m_permeability][lev])[dit](iv,0);
				Real permeabilityOld = (*m_scalarOld[m_permeability][lev])[dit](iv,0);


				Real thetaNew = (*m_scalarNew[m_theta][lev])[dit](iv,0);
				Real thetaOld = (*m_scalarOld[m_theta][lev])[dit](iv,0);


				Real ThetaNew = (*m_scalarNew[m_composition][lev])[dit](iv,0);
				Real ThetaOld = (*m_scalarOld[m_composition][lev])[dit](iv,0);

				permeability = (1-alpha) * permeabilityOld + alpha * permeabilityNew;
				theta = (1-alpha) * thetaOld + alpha * thetaNew;
				Theta = (1-alpha) * ThetaOld + alpha * ThetaNew;


				Real x = (iv[0]+0.5)*m_amrDx[lev];
				Real y = (iv[1]+0.5)*m_amrDx[lev];

				//				uStar[dit](iv, 0) = 0;
				//				uStar[dit](iv, 1) = 0;
				//
				//				if (iv[0] == 10 && iv[0] == 10)
				//				{
				//					uStar[dit](iv, 0) = 1;
				//				}
				uStar[dit](iv, 0) = 0;
				uStar[dit](iv, 1) = 0;

				//				if (iv[0] < 30  && iv[0] > 5 &&
				//					iv[1] < 30  && iv[1] > 5 )
				//				{
				//					uStar[dit](iv, 1) = theta;
				//				}

				/*
				 * Test case for BCs:
				 * Actually now dp/dx = 0 (x=0,1), dp/dz = 1 (z=0,1)
				 * P = cos(pi*x) * cos(pi*z) analytic solution
				 */
				uStar[dit](iv, 0) = - M_PI * cos(M_PI*y)*sin(M_PI*x);
				uStar[dit](iv, 1) =  - M_PI * sin(M_PI*y)*cos(M_PI*x);
				divUstarAnalytic[dit](iv, 0) = -2*M_PI*M_PI*cos(M_PI*y)*cos(M_PI*x);

				pressureErr[dit](iv,0) = cos(M_PI*x)*cos(M_PI*y) + 0.0232083;

				gradPErr[dit](iv,0) = -M_PI*sin(M_PI*x)*cos(M_PI*y);
				gradPErr[dit](iv,1) = -M_PI*cos(M_PI*x)*sin(M_PI*y);

				uAnalytic[dit](iv, 0) = 0;
				uAnalytic[dit](iv, 1) =  0;

				/*
				 * Test case for dp/dx = 0 (x=0,1) and dp/dz=-pi*cos(pi*x) (z=0,1) BCs
				 *cos(M_PI*x)				 * P = cos(pi*x) * sin(pi*z) analytic solution
				 */
				//				uStar[dit](iv, 0) = - M_PI * sin(M_PI*x)*sin(M_PI*y);
				//				uStar[dit](iv, 1) =  M_PI * cos(M_PI*y)*cos(M_PI*x);

				/*
				 * Test case for zero divergence
				 */
				//				uStar[dit](iv, 0) = cos(M_PI*4*x)*cos(M_PI*y);
				//				uStar[dit](iv, 1) = 4*sin(M_PI*y)*sin(M_PI*4*x);

				/*
				 * Actual problem
				 * U^*_x = 0
				 * U^*_z = permeability * (Ra_t * theta + Ra_C * Theta)
				 */

				//Perturbation comes from init
				Real perturbation = 0.1;

				uStar[dit](iv, 0)  = 0;
				uStar[dit](iv, 1) = permeability * ( m_parameters.rayleighTemp * theta +
						m_parameters.rayleighComposition * Theta);

				/*
				 * Create fields of analytic solutions
				 */

				divUstarAnalytic[dit](iv, 0) = m_parameters.rayleighTemp *(-1+perturbation*M_PI*cos(M_PI*(x))*cos(M_PI*(y)));

				pressureErr[dit](iv,0) = m_parameters.rayleighTemp *(y-0.5*y*y - (perturbation/(2*M_PI))*cos(M_PI*(x))*cos(M_PI*(y)));

				// Have to be careful here as we're comparing against edge centred data.
				x = x - m_amrDx[lev]/2;
				gradPErr[dit](iv,0) = m_parameters.rayleighTemp *(0.5*perturbation*sin(M_PI*(x))*cos(M_PI*(y)));

				x = x + m_amrDx[lev]/2; y = y - m_amrDx[lev]/2;
				gradPErr[dit](iv,1) = m_parameters.rayleighTemp *(1-y+0.5*perturbation*cos(M_PI*(x))*sin(M_PI*(y)));

				y = y + m_amrDx[lev]/2;



				//				thetaAdvectionError[dit](iv, 0) = m_parameters.rayleighTemp * (-1 + cos(M_PI*x) *
				//						(-M_PI * alpha * cos(M_PI*y) + 0.5  * alpha*sin(M_PI*y) )
				//						+ (M_PI/4) * alpha*alpha*sin(2*M_PI*y) );
				thetaAdvectionAnalytic[dit](iv, 0) = - m_parameters.rayleighTemp * perturbation * (
						- 0.5 * cos(M_PI*x) + 0.5 * M_PI * perturbation * cos(M_PI*y)) * sin(M_PI*y) ;



			} //finish loop over intvects in box

			logMessage(10, "    amrMushyLayerAdvance::updateVelocity - finished make U^* on grid");

			// Scale with filter
			//			for (int idir=0; idir<SpaceDim; idir++)
			//			{
			//				uStar[dit].mult((*m_scalarNew[m_buoyancyFilter][lev])[dit], 0, idir);
			//			}

		} // finish loop over boxes

		//Apply extrap BCs to U^* to ensure that div(U^*) is calculated correctly at the boundaries
		velBCExtrap.applyBCs(uStar, activeGrids[lev],
				m_amrDomains[lev], m_amrDx[lev],
				false); // inhomogeneous

	} // end loop over levels to setup U^*

	logMessage(10, "    amrMushyLayerAdvance::updateVelocity - do projection");
	// Project the predicted velocity onto the space of divergence free vectors

	CCProjectorComp proj;
	proj.define(activeGrids, m_amrDomains, m_amrDx, m_refinement_ratios, m_finest_level,
			*m_physBCPtr, m_scalarNew[m_theta], m_scalarNew[m_permeability], m_num_ghost);

	proj.projectVelocity(m_vectorNew[m_fluidVel], m_scalarNew[m_permeability],
			m_fluidAdv, m_vectorNew[m_fluidVelPred], m_fluidVelDiffOrder);

	logMessage(10, "    amrMushyLayerAdvance::updateVelocity - apply corrections");

	// Synchronization step
	//	logMessage(2, " updateVelocity - do synchronisation");

	// 1. Do velocity refluxing
	for (int lev=0; lev <= m_finest_level; lev++)
	{
		//		doImplicitVelocityReflux(lev);
		//		doExplicitVelocityReflux();
	}

	// 2. Ensure velocity is divergence free

	// Set lambda = 1, advect it, subtract 1. We should get zero - anything else is an error.
	Vector<LevelData<FArrayBox>* > zeroSource = newEmptyPtr();

	for (int lev=0; lev <= m_finest_level; lev++)
	{
		//		setValLevel(*m_scalarNew[m_lambda][lev], 1.0);
		for (DataIterator dit = m_scalarNew[m_lambda][lev]->dataIterator(); dit.ok(); ++dit)
		{
			(*m_scalarNew[m_lambda][lev])[dit].setVal(1.0);
			(*m_scalarOld[m_lambda][lev])[dit].setVal(1.0);
			(*m_scalarNew[m_lambdaPostCorr][lev])[dit].setVal(1.0);
			(*m_scalarOld[m_lambdaPostCorr][lev])[dit].setVal(1.0);
			(*zeroSource[lev])[dit].setVal(0.0);
		}
	}

	advectScalar(m_lambda, zeroSource, m_fluidAdv);

	Vector<LevelData<FArrayBox>* > a_lambda(m_finest_level+1, NULL);
	refCountedPtrToPtr(m_scalarNew[m_lambda], a_lambda);

	proj.doLambdaCorrection(m_fluidAdv, a_lambda, m_time, m_dt);

	//Re apply BCs after correction
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		edgeVelBC.applyBCs(*m_fluidAdv[lev], activeGrids[lev],
				m_amrDomains[lev], m_amrDx[lev],
				false); // inhomogeneous
	}

	advectScalar(m_lambdaPostCorr, zeroSource, m_fluidAdv);

	for (int lev=0; lev <= m_finest_level; lev++)
	{
		for (DataIterator dit = m_scalarNew[m_lambdaPostCorr][lev]->dataIterator(); dit.ok(); ++dit)
		{
			(*m_scalarNew[m_lambda][lev])[dit].plus(-1);
			(*m_scalarNew[m_lambdaPostCorr][lev])[dit].plus(-1);
		}
	}


	Vector<LevelData<FluxBox>* > gradPressureEdge;
	gradPressureEdge.resize(m_finest_level+1, NULL);
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		gradPressureEdge[lev] = new LevelData<FluxBox>(m_amrGrids[lev], 1, m_ghostVect - IntVect::Unit);
	}

	proj.getDivUStar(m_scalarNew[m_divUstar]);
	proj.getPressure(m_scalarNew[m_pressure]);
	proj.getGradPressure(m_vectorNew[m_gradPressure]);
	proj.getGradPressureEdge(gradPressureEdge);



	//Apply BCs
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		edgeVelBC.applyBCs(*m_fluidAdv[lev], activeGrids[lev],
				m_amrDomains[lev], m_amrDx[lev],
				false); // inhomogeneous
	}

	logMessage(10, "    amrMushyLayerAdvance::updateVelocity - calculate errors");

	//Calculate error in calculated quantities
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit = m_vectorNew[m_fluidVel][lev]->dataIterator(); dit.ok(); ++dit)
		{
			//Let's just enforce the analytic velocity and see if that works
			//			(*m_vectorNew[m_fluidVel][lev])[dit].setVal(0);
			//			(*m_vectorNew[m_fluidVel][lev])[dit] += (*m_vectorNew[m_fluidVelAnalytic][lev])[dit];

			(*m_scalarNew[m_divUstarErr][lev])[dit] -= (*m_scalarNew[m_divUstar][lev])[dit];

			//			(*m_vectorNew[m_gradPressureErr][lev])[dit] -= (*m_vectorNew[m_gradPressure][lev])[dit];

			//						(*m_vectorNew[m_fluidVelErr][lev])[dit].setVal(0);
			//						(*m_vectorNew[m_fluidVelErr][lev])[dit] += (*m_vectorNew[m_fluidVelAnalytic][lev])[dit];
			//						(*m_vectorNew[m_fluidVelErr][lev])[dit] -= (*m_vectorNew[m_fluidVel][lev])[dit];

			(*m_scalarNew[m_pressureError][lev])[dit] -= (*m_scalarNew[m_pressure][lev])[dit];


			FArrayBox& gradPerr = (*m_vectorNew[m_gradPressureErr][lev])[dit];
			FArrayBox gradPerry(gradPerr.box(), 1);
			gradPerry.copy(gradPerr, 1, 0, 1);

			FArrayBox Uerrx((*m_vectorNew[m_fluidVel][lev])[dit].box(), 1);
			FArrayBox Uerry((*m_vectorNew[m_fluidVel][lev])[dit].box(), 1);

			Uerrx.copy((*m_vectorNew[m_fluidVel][lev])[dit], 0, 0, 1);
			Uerry.copy((*m_vectorNew[m_fluidVel][lev])[dit], 1, 0, 1);

			Uerrx -= (*m_vectorNew[m_fluidVelErr][lev])[dit];
			Uerry.minus((*m_vectorNew[m_fluidVelErr][lev])[dit], 1, 0 , 1);

			// Calculate errors in edge centred quantities
			for (int idir = 0; idir<SpaceDim; idir++)
			{
				for (BoxIterator bit((*gradPressureEdge[lev])[dit][idir].box()); bit.ok(); ++bit)
				{
					IntVect ivEdge = bit();

					(*m_vectorNew[m_gradPressureErr][lev])[dit](ivEdge, idir) -= (*gradPressureEdge[lev])[dit][idir](ivEdge, 0);
					(*m_vectorNew[m_gradPressure][lev])[dit](ivEdge, idir) = (*gradPressureEdge[lev])[dit][idir](ivEdge, 0);
				}
			}

//			int temp=0;

		}

		Real maxPerr = computeMax(*m_scalarNew[m_pressureError][lev],
				&(m_scalarNew[m_pressure][lev]->disjointBoxLayout()),
				getRefinementRatio(lev));
		Real minPerr = computeMin(*m_scalarNew[m_pressureError][lev],
				&(m_scalarNew[m_pressure][lev]->disjointBoxLayout()),
				getRefinementRatio(lev));
		Real averagePerr = (maxPerr + minPerr)/2;

		for (DataIterator dit = m_scalarNew[m_pressureError][lev]->dataIterator(); dit.ok(); ++dit)
		{
			(*m_scalarNew[m_pressureError][lev])[dit].plus(-averagePerr);
		}


		m_vectorNew[m_fluidVel][lev]->exchange();
	}


	//Enforce analytic U edge if we want
	for (int lev=0; lev <=m_finest_level; lev++)
	{
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			FluxBox& advVel = (*m_fluidAdv[lev])[dit];
			FArrayBox& Ux = (*m_vectorNew[m_fluidVel][lev])[dit];
			FArrayBox Uz(Ux.box(),1);
			Uz.copy((*m_vectorNew[m_fluidVel][lev])[dit], 1,0,1);

			//Let's try filling our advVel fluxbox with the analytic solution
			FArrayBox& advVelx = advVel[0];
			FArrayBox& advVely = advVel[1];

			FArrayBox& advVelErr = (*m_vectorNew[m_fluidVelErr][lev])[dit];
			FArrayBox advVelErry(advVelErr.box(), 1);



			Box bx = advVelx.box();
			Box by = advVely.box();

			bx.grow(-m_num_ghost+1);
			BoxIterator bitx(bx);


			for (bitx.begin(); bitx.ok(); ++bitx)
			{
				IntVect iv = bitx();
				RealVect loc;
				getLocation(iv, lev, loc, 0, 0.5);

				Real x = loc[0]; Real y = loc[1];
				Real alpha = 0.1;


				//enforce velocity
				//				advVelx(iv, 0) = m_parameters.rayleighTemp *(-0.5*alpha*sin(M_PI*(x))*cos(M_PI*(y)));


				advVelErr(iv, 0) = m_parameters.rayleighTemp *(-0.5*alpha*sin(M_PI*(x))*cos(M_PI*(y)));
				advVelErr(iv, 0) = advVelErr(iv, 0) - advVelx(iv,0);

			}

			by.grow(-m_num_ghost+1);
			BoxIterator bity(by);


			for (bity.begin(); bity.ok(); ++bity)
			{
				IntVect iv = bity();
				RealVect loc;
				getLocation(iv, lev, loc, 0.5, 0);

				Real x = loc[0]; Real y = loc[1];
				Real alpha = 0.1;

				//enforce velocity
				//				advVely(iv, 0) =  m_parameters.rayleighTemp *(0.5*alpha*cos(M_PI*(x))*sin(M_PI*(y)));

				advVelErr(iv, 1) = m_parameters.rayleighTemp *(0.5*alpha*cos(M_PI*(x))*sin(M_PI*(y)));
				advVelErr(iv, 1) = advVelErr(iv, 1) - advVely(iv,0);

			}

			advVelErry.copy(advVelErr, 1, 0, 1);

//			int temp=0;

		} // end loop over grids



	} // end loop over levels



	// Need to clean up memory
	proj.undefine();


	//CF Interp test
	doCFInterpTest();

	//Calculate div(U)
	LevelData<FluxBox>* uEdgeFinePtr = NULL;
	Real* dxFine = NULL;
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		if (lev < m_finest_level)
		{
			uEdgeFinePtr = m_fluidAdv[lev+1];
			dxFine = &(m_amrDx[lev+1]);
		}
		else
		{
			uEdgeFinePtr = NULL;
			dxFine = NULL;
		}
		Divergence::compDivergenceMAC(*m_scalarNew[m_divU][lev], *m_fluidAdv[lev], uEdgeFinePtr,
				m_amrDx[lev],
				dxFine,
				getRefinementRatio(lev),
				m_amrDomains[lev]);
	}


	logMessage(10, "    amrMushyLayerAdvance::updateVelocity - Finished all levels");


	// Clean up memory
	for (int lev=0; lev<=m_finest_level; lev++){
		if(gradPressureEdge[lev]!=NULL)
		{
			delete gradPressureEdge[lev];
			gradPressureEdge[lev]  = NULL;
		}
		if(zeroSource[lev]!=NULL)
				{
					delete zeroSource[lev];
					zeroSource[lev]  = NULL;
				}

	}

}

void amrMushyLayer::
doCFInterpTest()
{

	//Set up initial field
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit=m_scalarNew[m_CFInterpTest][lev]->dataIterator(); dit.ok(); ++dit)
		{
			Box b = (*m_scalarNew[m_CFInterpTest][lev])[dit].box();
			FArrayBox& CFInterpTest = (*m_scalarNew[m_CFInterpTest][lev])[dit];
			for (BoxIterator bit = BoxIterator(b); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				RealVect loc;
				getLocation(iv, lev, loc);
				Real x = loc[0]; Real y = loc[1];
				CFInterpTest(iv,0) = sin(M_PI*x)*sin(M_PI*y);
			}
		}
	}

	// Do CF interp
	for (int lev=1; lev<=m_finest_level; lev++)
	{
		QuadCFInterp CFInterp(m_scalarNew[m_CFInterpTest][lev]->disjointBoxLayout(), &(m_scalarNew[m_CFInterpTest][lev-1]->disjointBoxLayout()),
				m_amrDx[lev], getRefinementRatio(lev-1),
				1, m_amrDomains[lev]);

		CFInterp.coarseFineInterp(*m_scalarNew[m_CFInterpTest][lev], *m_scalarNew[m_CFInterpTest][lev-1]);
	}

	// Calculate error
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit=m_scalarNew[m_CFInterpTest][lev]->dataIterator(); dit.ok(); ++dit)
		{
			Box b = (*m_scalarNew[m_CFInterpTest][lev])[dit].box();
			FArrayBox& CFInterpTest = (*m_scalarNew[m_CFInterpTest][lev])[dit];
			for (BoxIterator bit = BoxIterator(b); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				RealVect loc;
				getLocation(iv, lev, loc);
				Real x = loc[0]; Real y = loc[1];
				CFInterpTest(iv,0) -= sin(M_PI*x)*sin(M_PI*y);
			}

			if (lev > 0)
			{
//				int temp=0;
			}
		}
	}
}


bool amrMushyLayer::
residualsConstant(const Vector<Real> residuals)
{
	Real equal = 0.000001; // If two consecutive residuals are closer together than this, consider them equal

	for (int i=(residuals.size()-1); i > 0; i--)
	{
		if (abs(residuals[i]-residuals[i-1]) < equal)
		{
			// We're equal!
			return true;
		}
	}
	return false;
}


bool amrMushyLayer::
residualsDiverging(const Vector<Real> residuals)
{
	int numIncreasing = 0;
	int limit = 5; // How many times in a row can our residual be increasing before we decide it's diverging?

	for (int i=1; i < residuals.size(); i++)
	{
		if (residuals[i] > residuals[i-1])
		{
			numIncreasing++;
		}
		else
		{
			numIncreasing = 0;
		}

		if (numIncreasing == limit)
		{
			// We're diverging!
			return true;
		}
	}
	return false;
}

//Calculate the difference between two successive guesses of theta in the iteration
Real amrMushyLayer::
calculateHeatEqnResidual2(const Vector<LevelData<FArrayBox>* >a_thetaNew, const Vector<LevelData<FArrayBox>* > a_thetaNewPrev)
{
	Vector<LevelData<FArrayBox>* > thetaDiff(m_finest_level+1, NULL);

	//Calculate difference between two consecutive guesses of theta
	for(int lev=0; lev<=m_finest_level; lev++)
	{
		thetaDiff[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect);
		a_thetaNew[lev]->copyTo(*thetaDiff[lev]);

		for(DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*thetaDiff[lev])[dit] -= (*a_thetaNewPrev[lev])[dit];
		}
	}


	Real norm = computeNormWithSign(thetaDiff, m_refinement_ratios);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		if (thetaDiff[lev] != NULL)
		{
			delete thetaDiff[lev];

			//			thetaDiff[lev] == NULL;
		}
	}

	return norm;
}

Real amrMushyLayer::
calculateMaxDiff(Vector<LevelData<FArrayBox> * > a_phiNew, Vector<LevelData<FArrayBox> * > a_phiOld)
{
	Vector<RefCountedPtr<LevelData<FArrayBox> > > refPtrNew, refPtrOld;

	ptrToRefCountedPtr(a_phiNew, refPtrNew);
	ptrToRefCountedPtr(a_phiOld, refPtrOld);

	return calculateMaxDiff(refPtrNew, refPtrOld);
}

Real amrMushyLayer::
calculateDiffAtPoint(Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiNew,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiOld,
		int z_i)
{
	Real diff;
	for(int lev=0; lev<=m_finest_level; lev++)
	{
		for(DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			for (BoxIterator bit = BoxIterator(m_amrGrids[lev][dit]); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				if (iv[1] == z_i)
				{
					Real phiNewVal =  (*a_phiNew[lev])[dit](iv,0);
					Real phiOldVal =  (*a_phiOld[lev])[dit](iv,0);
					diff = phiNewVal - phiOldVal;
				}
			}

		}
	}
	return diff;

}
Real amrMushyLayer::
calculateMaxDiff(Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiNew, Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiOld)
{
	Vector<LevelData<FArrayBox>* > diff(m_finest_level+1, NULL);

	//Calculate difference between two consecutive guesses of theta
	for(int lev=0; lev<=m_finest_level; lev++)
	{
		diff[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect);
		a_phiNew[lev]->copyTo(*diff[lev]);

		for(DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*diff[lev])[dit] -= (*a_phiOld[lev])[dit];
		}
	}

	Interval comps(0,0);
	Real norm = computeNorm(diff, m_refinement_ratios, m_amrDx[0], comps, 0);

	return norm;
}



Real amrMushyLayer::
calculateResidual2(const Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiOld,
		const Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phiNew)
{
	Vector<LevelData<FArrayBox>* > residual;
	residual.resize(m_finest_level+1, NULL);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		residual[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, IntVect::Zero);
		a_phiNew[lev]->copyTo(*residual[lev]);

		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*residual[lev])[dit] -= (*a_phiOld[lev])[dit];
		}
	}


	Real norm = computeNormWithSign(residual, m_refinement_ratios);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		if (residual[lev] != NULL)
		{
			delete residual[lev];
			//			residual[lev] == NULL;
		}
	}

	return norm;

}

//Calculate dTheta/dt - ( V*d(Theta)/dz + (1/Le)*div(porosity*grad(Theta_l)) )
//Real amrMushyLayer::
//calculateConcEqnResidual()
//{
//
//	Vector<LevelData<FArrayBox>* > residual;
//	residual.resize(m_finest_level+1, NULL);
//
//	for (int lev=0; lev<=m_finest_level; lev++)
//	{
//		residual[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, IntVect::Zero);
//		LevelData<FArrayBox> residualSteady(m_amrGrids[lev], 1, IntVect::Zero);
//
//
//		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
//		{
//			(*residual[lev])[dit].setVal(0);
//			residualSteady[dit].setVal(0);
//
//			//Note that m_dScalar[m_composition] contains (V*d(Theta)/dz + (1/Le)*div(porosity*grad(Theta_l))) * m_dt
//			(*residual[lev])[dit()] += (*m_dScalar[m_composition][lev])[dit];
//
//			//Add [d(Theta)/dt] * m_dt
//			(*residual[lev])[dit()] += (*m_scalarNew[m_composition][lev])[dit()];
//			(*residual[lev])[dit()] -= (*m_scalarOld[m_composition][lev])[dit()];
//
//
//			//Divide by dt
//			(*residual[lev])[dit()].divide(m_dt);
//
//		}
//	}
//
//	Interval comps(0,0);
//
//	Real norm = computeNorm(residual, m_refinement_ratios, m_amrDx[0], comps, 0);
//
//	return norm;
//
//}

void amrMushyLayer::
applyBCs(const int a_var, const int lev)
{
	CH_TIME("amrMushyLayer::applyBCs");
	const DisjointBoxLayout& dbl = (*m_scalarNew[a_var][lev]).disjointBoxLayout();
	bool a_homogeneous = false;

	BCHolder bcHolder;

	//Get the BC function for this variable.
	if (a_var==m_enthalpy)
	{
		bcHolder = m_physBCPtr->enthalpyFuncBC();
	}
	else if(a_var == m_composition)
	{
		bcHolder = m_physBCPtr->ThetaFuncBC();
	}
	else if(a_var == m_porosity)
	{
		bcHolder = m_physBCPtr->porosityFuncBC();
		//				ABParseBCPorosity( (*m_scalarNew[a_var][lev])[dit], dbl[dit], m_amrDomains[lev], m_amrDx[lev], a_homogeneous);
	}
	else if(a_var == m_theta)
	{
		bcHolder = m_physBCPtr->thetaFuncBC();
	}
	else if(a_var == m_compositionLiquid)
	{
		bcHolder = m_physBCPtr->ThetaLFuncBC();
	}
	else if(a_var == m_compositionSolid)
	{
		bcHolder = m_physBCPtr->ThetaSFuncBC();
	}
	else
	{
		//let's not throw an error anymore
		//				MayDay::Error("applyBCs() - Can't calculate apply bcs for a_var specified");
		return;
	}



	DataIterator dit = m_amrGrids[lev].dataIterator();
	{
		for (dit.begin(); dit.ok(); ++dit)
		{
			Box b = dbl[dit];

			bcHolder.operator()((*m_scalarNew[a_var][lev])[dit],
					b,
					m_amrDomains[lev],
					m_amrDx[lev],
					a_homogeneous);

			bcHolder.operator()((*m_scalarOld[a_var][lev])[dit],
					b,
					m_amrDomains[lev],
					m_amrDx[lev],
					a_homogeneous);



			//Grow box b to include the ghost cells we want to fill
			b.grow(m_num_ghost);
			//Interval is the grid cells we want to interp into
			//0 is the ghost cell adjacent to boundary, which we already have
			ExtrapFillPatch patch(dbl, b, Interval(1,m_num_ghost));
			for (int dir=0; dir<SpaceDim; dir++)
			{
				patch.fillExtrap((*m_scalarNew[a_var][lev]), dir, 0, 1);
				patch.fillExtrap((*m_scalarOld[a_var][lev]), dir, 0, 1);
			}

		}
	}

}

void amrMushyLayer::applyBCs(int a_var)
{
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		applyBCs(a_var, lev);
	}
}

void amrMushyLayer::
applyVectorBCs(const int a_var, const int lev)
{
	CH_TIME("amrMushyLayer::applyVectorBCs");
	const DisjointBoxLayout& dbl = (*m_vectorNew[a_var][lev]).disjointBoxLayout();
//	bool a_homogeneous = false;

	VelBCHolder bcHolder;

	//Get the BC function for this variable.
	if (a_var==m_fluidVel)
	{
		bcHolder = m_physBCPtr->velFuncBC();
	}
	else
	{
		//let's not throw an error anymore
		//				MayDay::Error("applyBCs() - Can't calculate apply bcs for a_var specified");
		return;
	}

	bcHolder.applyBCs(*m_vectorNew[a_var][lev], m_amrGrids[lev],
			m_amrDomains[lev], m_amrDx[lev],
			false);


	DataIterator dit = m_amrGrids[lev].dataIterator();
	{

		Box b = m_amrGrids[lev][dit];
		//Grow box b to include the ghost cells we want to fill
		b.grow(m_num_ghost);
		//Interval is the grid cells we want to interp into
		//0 is the ghost cell adjacent to boundary, which we already have
		ExtrapFillPatch patch(dbl, b, Interval(1,m_num_ghost));
		for (int dir=0; dir<SpaceDim; dir++)
		{
			patch.fillExtrap((*m_vectorNew[a_var][lev]), dir, 0, SpaceDim);
			patch.fillExtrap((*m_vectorOld[a_var][lev]), dir, 0, SpaceDim);
		}


	}

}

void amrMushyLayer::
calculateFrameAdvectionGodunov(const int a_var)
{
	Vector<LevelData<FArrayBox>* > zeroSource = newEmptyPtr();

	advectScalar(a_var, zeroSource, m_frameAdv);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit = m_dScalar[a_var][lev]->dataIterator(); dit.ok(); ++dit)
		{
			(*m_dScalar[a_var][lev])[dit].setVal(0);
			(*m_dScalar[a_var][lev])[dit] += (*m_scalarNew[a_var][lev])[dit];
			(*m_dScalar[a_var][lev])[dit] -= (*m_scalarOld[a_var][lev])[dit];
			(*m_dScalar[a_var][lev])[dit] /= m_dt;
		}

		//Reset scalar new to undo the effects of advectScalar()
		m_scalarOld[a_var][lev]->copyTo(*m_scalarNew[a_var][lev]);
	}
	for (int lev=0; lev<=m_finest_level; lev++)
		{
	if(zeroSource[lev]!=NULL)
	{
		delete zeroSource[lev];
		zeroSource[lev] = NULL;
	}
		}
}


//This is a new method to explicitly calculate V*d(phi)/dz, as the
//method using a godunov update appears to be first order
void amrMushyLayer::
calculateFrameAdvection(const int a_var, Vector<LevelData<FArrayBox>* >& a_d_dz, Real frameAdvVel)
{
	CH_TIME("amrMushyLayer::calculateFrameAdvection()");

	Vector<LevelData<FArrayBox>* > gradient(m_finest_level+1,NULL);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		const DisjointBoxLayout& grids = m_amrGrids[lev];
		gradient[lev] = new LevelData<FArrayBox>(grids, 2,m_ghostVect);
		DataIterator dit = gradient[lev]->dataIterator();
		for (dit.begin(); dit.ok(); ++dit)
		{
			(*gradient[lev])[dit()].setVal(0);
		}
	}

	for (int lev=0; lev<=m_finest_level; lev++)
	{

		//		for (DataIterator dit = m_dScalar[a_var][lev]->dataIterator(); dit.ok(); ++dit)
		//		{
		//			FArrayBox& box = (*m_scalarNew[a_var][lev])[dit];
		//			int bal = 1;
		//		}

		//Make sure ghost cells are filled correctly before calculating gradient
		m_scalarNew[a_var][lev]->exchange();

		//Asumes BCS are set
		//Gradient::levelGradientCC(*gradient[lev], *m_scalarNew[a_var][lev], m_amrDx[lev]);

		//The upwinded version ensures we don't consider downwind information, which shouldn't be affecting
		//the solution
		Gradient::levelGradientCCUpwind(*gradient[lev], *m_scalarNew[a_var][lev], m_amrDx[lev]);

		for (DataIterator dit = m_dScalar[a_var][lev]->dataIterator(); dit.ok(); ++dit)
		{

			//multiply by non dimensional velocity
			(*gradient[lev])[dit].mult(frameAdvVel);

			//Take z-component of gradient and put in our dvar/dz
			BoxIterator bit(m_amrGrids[lev][dit]);
			for (bit.begin(); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				(*a_d_dz[lev])[dit](iv, 0) += (*gradient[lev])[dit](iv, 1);
			}
		}
	}

	//Clean up memory
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		if (gradient[lev] != NULL)
		{
			delete gradient[lev];
			gradient[lev] = NULL;
		}
	}
}

Vector<LevelData<FArrayBox>* > amrMushyLayer::
newEmptyPtr()
{

	Vector<LevelData<FArrayBox>* > phi(m_finest_level+1,NULL);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		const DisjointBoxLayout& grids = m_amrGrids[lev];
		phi[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);

		DataIterator dit = phi[lev]->dataIterator();
		for (dit.begin(); dit.ok(); ++dit)
		{
			(*phi[lev])[dit()].setVal(0);
		}
	}

	return phi;
}
Vector<RefCountedPtr<LevelData<FArrayBox> > > amrMushyLayer::
newEmptyRefPtr()
{

	Vector<RefCountedPtr<LevelData<FArrayBox> > > phi(m_finest_level+1);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		const DisjointBoxLayout& grids = m_amrGrids[lev];
		phi[lev] = RefCountedPtr<LevelData<FArrayBox> > (new LevelData<FArrayBox>(grids, 1,m_ghostVect));

		DataIterator dit = phi[lev]->dataIterator();
		for (dit.begin(); dit.ok(); ++dit)
		{
			(*phi[lev])[dit()].setVal(0);
		}
	}

	return phi;
}

void amrMushyLayer::
calculateThetaLSourceTerm(Vector<LevelData<FArrayBox>* > V_dThetadz_n, Vector<LevelData<FArrayBox>* > ThetaLSource)
{
	CH_TIME("amrMushLayer:calculateThetaLSourceTerm");
	//Best guess at frame advection at n+1
	Vector<LevelData<FArrayBox>* > V_dThetadz_nPlusOne = newEmptyPtr();

	//Theta_l^(n+1/2)
	Vector<LevelData<FArrayBox>* > averageThetaL = timeCenteredScalar(m_compositionLiquid);

	//Solid concentration source term
	Vector<LevelData<FArrayBox>* > ThetaSSource = newEmptyPtr();


	if (m_frameAdvectionMethod == m_finiteDifference)
	{
		calculateFrameAdvection(m_composition, V_dThetadz_nPlusOne, m_parameters.nonDimVel);
	}

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*m_dScalar[m_composition][lev])[dit].setVal(0);

			// Frame advection, averaged over times n+1 and n
			if (m_frameAdvectionMethod == m_finiteDifference)
			{
				(*m_dScalar[m_composition][lev])[dit].setVal(0);
				(*m_dScalar[m_composition][lev])[dit] += (*V_dThetadz_n[lev])[dit];
				(*m_dScalar[m_composition][lev])[dit] += (*V_dThetadz_nPlusOne[lev])[dit];
				(*m_dScalar[m_composition][lev])[dit] /= 2;
			}

			//Theta_s source term
			(*m_dScalar[m_compositionSolid][lev])[dit].setVal(1);
			(*m_dScalar[m_compositionSolid][lev])[dit].minus((*m_scalarNew[m_porosity][lev])[dit]);
			(*m_dScalar[m_compositionSolid][lev])[dit].mult((*m_scalarNew[m_compositionSolid][lev])[dit]);

			(*ThetaSSource[lev])[dit].setVal(1);
			(*ThetaSSource[lev])[dit].minus((*m_scalarOld[m_porosity][lev])[dit]);
			(*ThetaSSource[lev])[dit].mult((*m_scalarOld[m_compositionSolid][lev])[dit]);

			(*m_scalarNew[m_ThetaSSource][lev])[dit].setVal(0);
			(*m_scalarNew[m_ThetaSSource][lev])[dit] += (*m_dScalar[m_compositionSolid][lev])[dit];
			(*m_scalarNew[m_ThetaSSource][lev])[dit] -= (*ThetaSSource[lev])[dit];
			(*m_scalarNew[m_ThetaSSource][lev])[dit].divide(m_dt);
			(*m_scalarNew[m_ThetaSSource][lev])[dit].mult(-1);

			//Porosity source term
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit].setVal(0);
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit] += (*m_scalarNew[m_porosity][lev])[dit];
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit] -= (*m_scalarOld[m_porosity][lev])[dit];
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit] /= m_dt;
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit].mult((*averageThetaL[lev])[dit]);
			(*m_scalarNew[m_ThetaPorositySource][lev])[dit].mult(-1);

			//Put it all together, praying that all the signs are correct
			(*ThetaLSource[lev])[dit].setVal(0);
			(*ThetaLSource[lev])[dit] += (*m_dScalar[m_composition][lev])[dit];
			(*ThetaLSource[lev])[dit] += (*m_scalarNew[m_ThetaPorositySource][lev])[dit];
			(*ThetaLSource[lev])[dit] += (*m_scalarNew[m_ThetaSSource][lev])[dit];

			//For debugging
			(*m_scalarNew[m_concSource][lev])[dit].setVal(0);
			(*m_scalarNew[m_concSource][lev])[dit] += (*ThetaLSource[lev])[dit];

		}
		ThetaLSource[lev]->exchange();
	}

	//Clean up memory
	for (int lev=0; lev <= m_finest_level; lev++)
	{
		if (V_dThetadz_nPlusOne[lev] != NULL)
		{
			delete V_dThetadz_nPlusOne[lev];
			V_dThetadz_nPlusOne[lev] = NULL;
		}
		if (averageThetaL[lev] != NULL)
		{
			delete averageThetaL[lev];
			averageThetaL[lev] = NULL;
		}
		if (ThetaSSource[lev] != NULL)
		{
			delete ThetaSSource[lev];
			ThetaSSource[lev] = NULL;
		}

	}


}

void amrMushyLayer::
calculatethetaSourceTerm(Vector<LevelData<FArrayBox>* > thetaSource,  Vector<LevelData<FArrayBox>* > V_gradH_n)
{
	CH_TIME("amrMushyLayer::calclulatethetaSouceTerm");
	//Calculate enthalpy advection at n+1
	Vector<LevelData<FArrayBox>* > V_gradH_nPlusOne(m_finest_level+1,NULL);

	//contains theta*(c_p-1) - S
	Vector<LevelData<FArrayBox>* > porosityCoeff(m_finest_level+1,NULL);

	Vector<LevelData<FArrayBox>* > fakePorositySource(m_finest_level+1,NULL);

	Vector<LevelData<FArrayBox>* > thetaAverage = timeCenteredScalar(m_theta);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		const DisjointBoxLayout& grids = m_amrGrids[lev];
		V_gradH_nPlusOne[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		porosityCoeff[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);
		fakePorositySource[lev] = new LevelData<FArrayBox>(grids, 1,m_ghostVect);

		DataIterator dit = V_gradH_nPlusOne[lev]->dataIterator();
		for (dit.begin(); dit.ok(); ++dit)
		{
			(*V_gradH_nPlusOne[lev])[dit()].setVal(0);
			(*porosityCoeff[lev])[dit()].setVal(0);
			(*fakePorositySource[lev])[dit()].setVal(0);
		}
	}

	if (m_frameAdvectionMethod == m_finiteDifference)
	{
		calculateFrameAdvection(m_enthalpy, V_gradH_nPlusOne, m_parameters.nonDimVel);
		calculateFrameAdvection(m_porosity, fakePorositySource, 1); // V = 1
	}


	//	Real heleShawCooling = m_parameters.nonDimHeleShawCooling * m_parameters.thetaInf;

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			// This is V * S *d (porosity)/dz
			(*fakePorositySource[lev])[dit()].mult(0);

			// Calculate (theta*(c_p - 1) - S)*d(porosity)/dt
			(*porosityCoeff[lev])[dit()].setVal(-1);
			(*porosityCoeff[lev])[dit()].plus(m_parameters.specificHeatRatio);
			(*porosityCoeff[lev])[dit()].mult((*thetaAverage[lev])[dit]);
			(*porosityCoeff[lev])[dit()].plus(-m_parameters.stefan);

			(*m_dScalar[m_porosity][lev])[dit()].setVal(0);
			(*m_dScalar[m_porosity][lev])[dit()] += (*m_scalarNew[m_porosity][lev])[dit()];
			(*m_dScalar[m_porosity][lev])[dit()] -= (*m_scalarOld[m_porosity][lev])[dit()];
			(*m_dScalar[m_porosity][lev])[dit()] /= m_dt;
			//			(*m_dScalar[m_porosity][lev])[dit()] *= m_parameters.stefan;
			(*m_dScalar[m_porosity][lev])[dit()].mult((*porosityCoeff[lev])[dit()]);

			//Calculate d/dz (H^n + H^(n+1) )/2
			if (m_frameAdvectionMethod == m_finiteDifference)
			{
				(*m_dScalar[m_enthalpy][lev])[dit()].setVal(0);
				(*m_dScalar[m_enthalpy][lev])[dit()] += (*V_gradH_n[lev])[dit];
				(*m_dScalar[m_enthalpy][lev])[dit()] += (*V_gradH_nPlusOne[lev])[dit];
				(*m_dScalar[m_enthalpy][lev])[dit()] /= 2;
			}


			//Build the full forcing term (theta*(c_p-1) -S)*d(porosity)/dt + V*dH/dz
			(*thetaSource[lev])[dit()].setVal(0);
			(*thetaSource[lev])[dit()] += (*m_dScalar[m_porosity][lev])[dit()];
			(*thetaSource[lev])[dit()] += (*m_dScalar[m_enthalpy][lev])[dit()];
			(*thetaSource[lev])[dit()] += (*m_dScalar[m_theta][lev])[dit()];
			(*thetaSource[lev])[dit()] += (*fakePorositySource[lev])[dit()];

		}
		m_dScalar[m_enthalpy][lev]->copyTo(*m_scalarNew[m_enthalpyAdvection][lev]);

		thetaSource[lev]->exchange();

		//Stick the forcing term in here for debugging
		thetaSource[lev]->copyTo(*m_scalarNew[m_thetaForcing][lev]);

		//Also calculate error in this term
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*m_scalarNew[m_thetaForcingError][lev])[dit].setVal(0);
			(*m_scalarNew[m_thetaForcingError][lev])[dit] += (*m_scalarNew[m_thetaForcingAnalytic][lev])[dit];
			(*m_scalarNew[m_thetaForcingError][lev])[dit] -= (*m_scalarNew[m_thetaForcing][lev])[dit];
		}
	}

	//Clean up memory
	for (int lev=0; lev <= m_finest_level; lev++)
	{
		if (V_gradH_nPlusOne[lev] != NULL)
		{
			delete V_gradH_nPlusOne[lev];
			V_gradH_nPlusOne[lev] = NULL;
		}
		if (porosityCoeff[lev] != NULL)
		{
			delete porosityCoeff[lev];
			porosityCoeff[lev] = NULL;
		}
		if (thetaAverage[lev] != NULL)
		{
			delete thetaAverage[lev];
			thetaAverage[lev] = NULL;
		}
		if (fakePorositySource[lev] != NULL)
		{
			delete fakePorositySource[lev];
			fakePorositySource[lev] = NULL;
		}




	}
}


//Calculate (1/Le) * grad (porosity* grad Theta_l)
void amrMushyLayer::
calculateThetaDiffusion(Vector<LevelData<FArrayBox>* > a_ThetaDiffusion)
{
	CH_TIME("amrMushyLayer::calculateThetaDiffusion()");
	Vector<LevelData<FArrayBox>* > zero;
	zero.resize(m_finest_level+1, NULL);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		zero[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_num_ghost*IntVect::Unit);
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*zero[lev])[dit].setVal(0);
		}
	}

	for (int lev=0; lev<=m_finest_level; lev++)
	{

		RefCountedPtr<VCAMRPoissonOp2> thisamrpop;

		thisamrpop = RefCountedPtr<VCAMRPoissonOp2>((VCAMRPoissonOp2*)ThetaLVCOpFact->AMRnewOp(m_amrDomains[lev]));

		Real alpha, beta;
		Vector<RefCountedPtr<LevelData<FArrayBox> > > aCoef;
		Vector<RefCountedPtr<LevelData<FluxBox> > > bCoef;


		getThetaLOpCoeffs(alpha, beta, aCoef, bCoef);

		for (DataIterator dit=aCoef[lev]->dataIterator(); dit.ok(); ++dit)
		{
			(*aCoef[lev])[dit].setVal(0);
		}


		//thisamrpop->setAlphaAndBeta(1.0, 1.0);
		thisamrpop->setCoefs(aCoef[lev], bCoef[lev], alpha, beta);
		thisamrpop->resetLambda();

		thisamrpop->residualI(*a_ThetaDiffusion[lev], *m_scalarNew[m_compositionLiquid][lev], *zero[lev]);

		thisamrpop->scale(*a_ThetaDiffusion[lev], -1.0);
		a_ThetaDiffusion[lev]->exchange();

		a_ThetaDiffusion[lev]->copyTo(*m_scalarNew[m_ThetaDiffusion][lev]);




	}

	for (int lev=0; lev <= m_finest_level; lev++)
	{
		if (zero[lev] != NULL)
		{
			delete zero[lev];
			zero[lev] = NULL;
		}


	}

}

//Calculate lap(theta) by advancing the heat equation with no source term
//currently unused
void amrMushyLayer::
calculatethetaDiffusion(
		Vector<RefCountedPtr<LevelData<FArrayBox> > >& a_thetaDiffusion,
		Vector<LevelData<FArrayBox>* >& a_thetaOld)
{
	CH_assert(m_timeIntegrationOrder <= 2);

	int lbase = 0;
	int lmax = m_finest_level;

	Vector<LevelData<FArrayBox>* > zeroSrc(m_finest_level+1, NULL);
	Vector<LevelData<FArrayBox>* > diffusionPtr(m_finest_level+1, NULL);
	Vector<LevelData<FArrayBox>* > a_thetaNew(m_finest_level+1, NULL);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		zeroSrc[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect);
		a_thetaNew[lev] = new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect);

		for (DataIterator dit=m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*zeroSrc[lev])[dit].setVal(0);
		}

		diffusionPtr[lev] = (&*a_thetaDiffusion[lev]);
	}



	if (m_timeIntegrationOrder == 1)
	{
		//For backward euler, we use lap(theta)^(n+1) in the timestepping
		for (int lev=0; lev<=m_finest_level; lev++)
		{
			LevelData<FArrayBox> thetaLaplacian(m_amrGrids[lev], 1, IntVect::Zero);
			makeLaplacian(m_theta, lev, *diffusionPtr[lev]);
		}
		//m_diffuseBEtheta->computeDiffusion(diffusionPtr, a_thetaOld, zeroSrc, m_time, m_dt, lbase, lmax, false);
	}
	else if(m_timeIntegrationOrder == 2)
	{
		//For TGA, we use lap(theta)^(n+1/2) in the timestepping

		//		m_diffuseTGAtheta->oneStep(a_thetaNew, a_thetaOld, zeroSrc, m_dt, lbase, lmax, m_time);
		//
		//		//Calculate difference between old and new theta
		//		for (int lev=0; lev<=m_finest_level; lev++)
		//		{
		//			a_thetaNew[lev]->copyTo(*diffusionPtr[lev]);
		//
		//			for (DataIterator dit=m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		//			{
		//				(*diffusionPtr[lev])[dit] -= (*a_thetaOld[lev])[dit];
		//				(*diffusionPtr[lev])[dit] /= m_dt;
		//			}
		//		}

		// This should give us a time-centered diffusion
		m_diffuseBEtheta->computeDiffusion(diffusionPtr, a_thetaOld, zeroSrc, m_time, m_dt, lbase, lmax, false);


	}

}


void amrMushyLayer::
makeLaplacian(const int a_var, const int lev, LevelData<FArrayBox>& a_laplacian)
{
	CH_TIME("amrMushyLayer::makeLaplacian()");

	LevelData<FArrayBox> zero;

	RefCountedPtr<AMRPoissonOp> thisamrpop;
	AMRLevelOp<LevelData<FArrayBox> >* finerOp;

	if (a_var == m_theta)
	{
		//		thisamrpop = RefCountedPtr<AMRPoissonOp>((AMRPoissonOp*)thetaPoissonOpFact->AMRnewOp(m_amrDomains[lev]));
		//		if (lev < m_finest_level)
		//		{
		//			finerOp = thetaPoissonOpFact->AMRnewOp(m_amrDomains[lev+1]);
		//		}

		thisamrpop = RefCountedPtr<AMRPoissonOp>((AMRPoissonOp*)thetaVCOpFact->AMRnewOp(m_amrDomains[lev]));
		if (lev < m_finest_level)
		{
			finerOp = thetaVCOpFact->AMRnewOp(m_amrDomains[lev+1]);
		}

		//thetaVCOpFact
	}
	else
	{
		MayDay::Error("amrMushyLayer::makeLaplacian() don't know how to deal with a_var");
	}

	thisamrpop->create(zero, a_laplacian);
	thisamrpop->setToZero(zero);
	thisamrpop->setAlphaAndBeta(0.0, 1.0);

	if(m_finest_level==0)
	{
		thisamrpop->residual(a_laplacian, *m_scalarNew[a_var][lev], zero, false);
	}
	else if (lev == 0)
	{
		thisamrpop->AMRResidualNC(a_laplacian, *m_scalarNew[a_var][lev+1], *m_scalarNew[a_var][lev], zero, false, finerOp);
	}
	else if(lev == m_finest_level)
	{
		thisamrpop->AMRResidualNF(a_laplacian, *m_scalarNew[a_var][lev], *m_scalarNew[a_var][lev-1], zero, false);
	}
	else
	{
		thisamrpop->AMRResidual(a_laplacian, *m_scalarNew[a_var][lev+1], *m_scalarNew[a_var][lev], *m_scalarNew[a_var][lev-1],
				zero, false, finerOp);
	}

	thisamrpop->scale(a_laplacian, -1.0);
	a_laplacian.exchange();
}

void amrMushyLayer::makeAdvectionTerm(const int a_var,
		const Vector<LevelData<FArrayBox>* > a_source,
		const Vector<LevelData<FluxBox>* > a_advVel,
		Vector<RefCountedPtr<LevelData<FArrayBox> > > a_advTerm)
{
	Interval interv(0,0);

	//First get a backup of m_scalarNew, and put m_scalarOld into m_scalarNew so it can be advected
	Vector<RefCountedPtr<LevelData<FArrayBox> > > scalarNewBackup;
	scalarNewBackup.resize(m_finest_level+1);
	for (int lev=0; lev<=m_finest_level; lev++)
	{
		scalarNewBackup[lev] = RefCountedPtr<LevelData<FArrayBox> >
		(new LevelData<FArrayBox>(m_amrGrids[lev], 1,m_ghostVect));

		m_scalarNew[a_var][lev]->copyTo(interv, *scalarNewBackup[lev], interv);
		m_scalarOld[a_var][lev]->copyTo(interv, *m_scalarNew[a_var][lev], interv);
	}

	advectScalar(a_var, a_source, a_advVel);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		//TODO: can we remove these exchange calls?
		m_scalarNew[a_var][lev]->exchange();
		m_scalarOld[a_var][lev]->exchange();

		m_scalarNew[a_var][lev]->copyTo(interv, *a_advTerm[lev], interv);

		//for (DataIterator dit = m_dScalar[a_var][lev]->dataIterator(); dit.ok(); ++dit)
		for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*a_advTerm[lev])[dit] -= (*m_scalarOld[a_var][lev])[dit];
			(*a_advTerm[lev])[dit] /= m_dt;
		}
		a_advTerm[lev]->exchange();
		//Put things back where they should be
		//			m_scalarOld[a_var][lev]->copyTo(interv, *m_scalarNew[a_var][lev], interv);
		scalarNewBackup[lev]->copyTo(interv, *m_scalarNew[a_var][lev], interv);
	}

}


void
amrMushyLayer::advectScalar(int a_var,
		Vector<LevelData<FArrayBox>* > a_source,
		Vector<LevelData<FluxBox>* > a_advVel)
{
	for (int lev = 0; lev<=m_finest_level; lev++)
	{

		RefCountedPtr<LevelFluxRegister> coarserFRPtr, finerFRPtr;
		RefCountedPtr<LevelData<FArrayBox> > coarserDataOldPtr, coarserDataNewPtr;
		Real tCoarserNew, tCoarserOld;

		LevelAdvect levAdvect;
		DisjointBoxLayout coarseGrid = DisjointBoxLayout();
		int refRatio; //These live on the coarser level
		bool use_limiting = true;
		bool hasCoarser = (lev > 0);
		bool hasFiner = (lev < m_finest_level);

		ParmParse pp("godunov");
		pp.query("use_limiting", use_limiting);

		getLevelAdvectionsVars(a_var, lev, refRatio, hasCoarser, hasFiner, use_limiting,
				coarserDataOldPtr, coarserDataNewPtr,
				coarserFRPtr, finerFRPtr, coarseGrid, levAdvect,
				tCoarserNew, tCoarserOld);

		m_scalarNew[a_var][lev]->exchange();

		levAdvect.step(*m_scalarNew[a_var][lev],
				*finerFRPtr,
				*coarserFRPtr,
				*a_advVel[lev],
				*a_source[lev],
				*coarserDataOldPtr,
				tCoarserOld,
				*coarserDataNewPtr,
				tCoarserNew,
				m_time,
				m_dt);

	}


}

void
amrMushyLayer::getActiveGridsVars(Vector<Vector<RefCountedPtr<LevelData<FArrayBox> > > >& a_scalarNew,
		Vector<Vector<RefCountedPtr<LevelData<FArrayBox> > > >& a_scalarOld,
		Vector<DisjointBoxLayout>&a_amrGrids)
{
	CH_TIME("amrMushyLayer::getActiveGridsVars");
	a_scalarNew.resize(m_numVars);
	a_scalarOld.resize(m_numVars);
	a_amrGrids.resize(m_finest_level+1);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		a_amrGrids[lev] = m_amrGrids[lev];
	}

	for (int a_var=0; a_var<m_numVars; a_var++)
	{
		a_scalarNew[a_var].resize(m_finest_level+1);
		a_scalarOld[a_var].resize(m_finest_level+1);

		for (int lev=0; lev<=m_finest_level; lev++)
		{
			//These are just pointers
			a_scalarNew[a_var][lev] = m_scalarNew[a_var][lev];
			a_scalarOld[a_var][lev] = m_scalarOld[a_var][lev];
		}
	}
}


void
amrMushyLayer::getFrameAdvection(const int lev)
{
	//Delete the old data to clean up memory
	if (m_frameAdv[lev] != NULL)
	{
		delete m_frameAdv[lev];
		m_frameAdv[lev] = NULL;
	}

	// Now create new data
	m_frameAdv[lev] = new LevelData<FluxBox>(m_amrGrids[lev], 1, m_num_ghost*IntVect::Unit);

	for (DataIterator dit = m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
	{
		// The current box
		Box curBox = m_amrGrids[lev].get(dit());

		FluxBox& advVelFluxBox = (*m_frameAdv[lev])[dit];
		for (int faceDir=0; faceDir < SpaceDim; faceDir++)
		{
			Real vel;
			if (faceDir == 1)
			{
				// Not the minus sign here
				// Fluid is pulled down towards the cold heat exchanger
				vel = - m_parameters.nonDimVel;
			}
			else
			{
				vel = 0;
			}
			FArrayBox& velDir = advVelFluxBox[faceDir];
			velDir.setVal(vel);
		}
	}
}


//a_coef = 1
//b_coef = permeability
void amrMushyLayer::
getPressureVCcoefs(Vector<RefCountedPtr<LevelData<FArrayBox> > >& aCoef,
		Vector<RefCountedPtr<LevelData<FluxBox> > >& bCoef)
{
	aCoef.resize(m_finest_level+1);
	bCoef.resize(m_finest_level+1);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		aCoef[lev] = RefCountedPtr<LevelData<FArrayBox> >
		(new LevelData<FArrayBox>(m_amrGrids[lev], 1, m_ghostVect ));

		bCoef[lev] = RefCountedPtr<LevelData<FluxBox> >
		(new LevelData<FluxBox>(m_amrGrids[lev], 1, m_ghostVect));

		bCoef[lev].neverDelete();

		LevelData<FluxBox> *fluxBox = &(*bCoef[lev]);

		//Create FArray box of a and b coefficients
		for (DataIterator dit=m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*aCoef[lev])[dit].setVal(0);

			FluxBox& fbox = (*fluxBox)[dit];
			for (int faceDir=0; faceDir<SpaceDim; faceDir++)
			{
				FArrayBox& fluxDir = fbox[faceDir];
				fluxDir.setVal(-1);
			}
		}

		aCoef[lev]->exchange();
		bCoef[lev]->exchange();

		//		//Turn FArrayBox into FluxBox for B coefficient
		//LevelData<FluxBox> *fluxBox = &(*bCoef[lev]);
		CellToEdge(*m_scalarNew[m_permeability][lev], *fluxBox);


		// Multiply bCoef fluxbox by -1 so signs are correct
		for (DataIterator dit=m_amrGrids[lev].dataIterator(); dit.ok(); ++dit)
		{
			(*fluxBox)[dit] *= -1;
		}

	}
}




void
amrMushyLayer::getFluidAdvection(const int lev)
{
	logMessage(10, "    amrMushyLayerAdvance::getFluidAdvection");
	//Delete the old data to clean up memory
	if (m_fluidAdv[lev] != NULL)
	{
		delete m_fluidAdv[lev];
		m_fluidAdv[lev] = NULL;
	}

	// Now create new data
	m_fluidAdv[lev] = new LevelData<FluxBox>(m_amrGrids[lev], 1, m_ghostVect);

	//Set everything to zero
	for (DataIterator dit = m_vectorNew[m_fluidVel][lev]->dataIterator(); dit.ok(); ++dit)
	{
		FluxBox& temp_advVel = (*m_fluidAdv[lev])[dit];
		for (int faceDir=0; faceDir < SpaceDim; faceDir++)
		{
			Real vel = 0;

			FArrayBox& velDir = temp_advVel[faceDir];
			velDir.setVal(vel);
		}
		//		int temp=0;
	}

	// Get velocities from the FArrayBox
	m_vectorNew[m_fluidVel][lev]->exchange();
	CellToEdge(*m_vectorNew[m_fluidVel][lev], *m_fluidAdv[lev]);

	m_fluidAdv[lev]->exchange();

	//Do my own quadratic interpolation!!

	for (DataIterator dit = m_vectorNew[m_fluidVel][lev]->dataIterator(); dit.ok(); ++dit)
	{
		//			Box b =  (*m_vectorNew[m_fluidVel][lev])[dit].box();
		Box b = m_amrGrids[lev][dit];

		FluxBox& edgeData  = (*m_fluidAdv[lev])[dit];

		for (int idir=0; idir<SpaceDim; idir++)
		{
			b.growHi(idir, -1);

			FArrayBox& fluidAdvDir =edgeData[idir];

			for (BoxIterator bit = BoxIterator(b); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				Real y0 = (*m_vectorNew[m_fluidVel][lev])[dit](iv - BASISV(idir), idir);
				Real y1 = (*m_vectorNew[m_fluidVel][lev])[dit](iv, idir);
				Real y2 = (*m_vectorNew[m_fluidVel][lev])[dit](iv + BASISV(idir), idir);

				if (m_fluidVelInterpOrder == 2)
				{
					fluidAdvDir(iv, 0) =  (3*y0 + 6*y1 - y2)/8;  //2nd order
				}
				else if (m_fluidVelInterpOrder == 1)
				{
					fluidAdvDir(iv, 0) =  (y0+y1)/2; //1st order
				}
				else
				{
					fluidAdvDir(iv, 0) =  y1; //0th order
				}

			}

			Box end = adjCellBox(b, idir, Side::Hi , 2);

			for (BoxIterator bit = BoxIterator(end); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				Real y0 = (*m_vectorNew[m_fluidVel][lev])[dit](iv - 2*BASISV(idir), idir);
				Real y1 = (*m_vectorNew[m_fluidVel][lev])[dit](iv - BASISV(idir), idir);
				Real y2 = (*m_vectorNew[m_fluidVel][lev])[dit](iv, idir);

				if (m_fluidVelInterpOrder == 2)
				{
					fluidAdvDir(iv, 0) =  (-y0 + 6*y1 + 3*y2)/8;  //2nd order
				}
				else if (m_fluidVelInterpOrder == 1)
				{
					fluidAdvDir(iv, 0) =  (y1+y2)/2; //1st order
				}
				else
				{
					fluidAdvDir(iv, 0) =  y1; //0th order
				}

			}

			b.growHi(idir, 1);

		}
	}




	//	for (DataIterator dit = m_vectorNew[m_fluidVel][lev]->dataIterator(); dit.ok(); ++dit)
	//		{
	//
	//			FluxBox& advFlux = (*m_fluidAdv[lev])[dit];
	//			FArrayBox& Ux = (*m_vectorNew[m_fluidVel][lev])[dit];
	//			FArrayBox& Uz = (*m_vectorNew[m_fluidVel][lev])[dit];
	//			Uz.copy((*m_vectorNew[m_fluidVel][lev])[dit], 1,0,1);
	//				int temp=0;
	//		}

}



void amrMushyLayer::removeNanValues(FArrayBox& fab)
{
	Box b = fab.box();

	for (BoxIterator bit = BoxIterator(b); bit.ok(); ++bit)
	{
		IntVect iv = bit();
		if (isnan(fab(iv,0)))
		{
			fab(iv,0) = 0;
		}
	}
}
void amrMushyLayer::enforceAbsCap(Vector<RefCountedPtr<LevelData<FArrayBox> > > a_phi, Real a_cap)
{
	//Ensure this is positive
	a_cap = abs(a_cap);

	for (int lev=0; lev<=m_finest_level; lev++)
	{
		for (DataIterator dit=a_phi[lev]->dataIterator(); dit.ok(); ++dit)
		{
			Box b = m_amrGrids[lev][dit];
			for (BoxIterator bit = BoxIterator(b); bit.ok(); ++bit)
			{
				IntVect iv = bit();
				Real phiVal = (*a_phi[lev])[dit](iv,0);
				if (abs(phiVal) > a_cap)
				{
					//Need to enforce the cap with the correct sign
					Real sign = 1;
					if (phiVal < 0)
					{
						sign = -1;
					}

					(*a_phi[lev])[dit](iv,0) = sign*a_cap;
				}
				else if (isnan(phiVal))
				{
					(*a_phi[lev])[dit](iv,0) = 0;
				}
			}
		}
	}
}
